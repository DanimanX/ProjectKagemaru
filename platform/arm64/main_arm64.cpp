// ProjectKagemaru — Virtua Fighter (vf1-recomp) ARM64 / OpenGL ES 2.0 / SDL2 Frontend
// Engineered for Anbernic RG34XX (Allwinner H700: 4x Cortex-A53 @ 1.5 GHz,
// Mali-G31 MP2, 1 GB LPDDR4, 720x480 3:2) and compatible PortMaster handhelds.
//
// * Core 0: Main loop, render_parts snapshot, GLES 2.0 + Swap
// * Core 1: EmuWorker (SLJIT ARM64 V60 + MB86233 TGP + Model 1 I/O)
// * Core 2: Decoupled SoundWorker (68000 @ 10 MHz + dual MultiPCM + optional YM3438)
// * Core 3: SDL2 adaptive-rate audio callback + auxiliary row_pool worker
#define SDL_MAIN_HANDLED

#include "audio.h"
#include "controls.h"
#include "gpu_gles.h"
#include "imgui_gles.h"
#include "sound_worker.h"
#include "thread_affinity.h"

#include "gpu/gpu3d.h"
#include "host/backgrounds.h"
#include "host/romset.h"
#include "machine/model1.h"
#include "rt/native.h"
#include "rt/rec_rt.h"

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kMainCore = 0;
constexpr int kEmuCore = 1;
constexpr int kSoundCore = 2;

uint64_t ticks_us() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

class EmuWorker {
public:
    EmuWorker() = default;
    ~EmuWorker() { close(); }

    void open(model1 *game, int core) {
        close();
        game_ = game;
        core_ = core;
        stop_ = false;
        busy_ = false;
        render_busy_ = false;
        game_->set_ext_render_wait([this]() { wait_render_done(); });
        thread_ = std::thread(&EmuWorker::loop, this);
    }

    void close() {
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                stop_ = true;
                render_busy_ = false;
            }
            cv_.notify_all();
            render_cv_.notify_all();
            thread_.join();
        }
        if (game_) {
            game_->set_ext_render_wait({});
            game_ = nullptr;
        }
    }

    void start_frame(bool render_in_progress) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            render_busy_ = render_in_progress;
            busy_ = true;
        }
        cv_.notify_one();
    }

    void mark_render_done() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!render_busy_) return;
            render_busy_ = false;
        }
        render_cv_.notify_all();
    }

    void wait_frame() {
        if (!thread_.joinable()) return;
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [&] { return !busy_; });
    }

    bool is_busy() {
        std::lock_guard<std::mutex> lk(mu_);
        return busy_;
    }

    double last_emu_ms() const { return last_emu_ms_.load(std::memory_order_relaxed); }

private:
    void wait_render_done() {
        std::unique_lock<std::mutex> lk(mu_);
        render_cv_.wait(lk, [&] { return !render_busy_ || stop_; });
    }

    void loop() {
        arm64::pin_current_thread(core_);
        arm64::set_current_thread_name("kagemaru-emu");
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || busy_; });
                if (stop_) return;
            }
            const uint64_t t0 = ticks_us();
            game_->run_frame();
            const double ms = double(ticks_us() - t0) / 1000.0;
            last_emu_ms_.store(ms, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mu_);
                busy_ = false;
            }
            done_.notify_all();
        }
    }

    model1 *game_ = nullptr;
    int core_ = 1;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_, done_, render_cv_;
    bool stop_ = false;
    bool busy_ = false;
    bool render_busy_ = false;
    std::atomic<double> last_emu_ms_{0.0};
};

void ensure_dir(const std::string &path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

std::string executable_dir(const char *argv0) {
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = (n > 0) ? std::string(buf, size_t(n)) : (argv0 ? std::string(argv0) : std::string("."));
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(data, 1, size, f) == size && std::fgetc(f) == EOF;
    std::fclose(f);
    return ok;
}

bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string load_text(const std::string &path) {
    std::ifstream f(path);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void save_text(const std::string &path, const std::string &txt) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = txt.empty() || std::fwrite(txt.data(), 1, txt.size(), f) == txt.size();
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { std::remove(tmp.c_str()); return; }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

constexpr const char *kBindKeys[arm64::ActionCount] = {
    "bind_guard", "bind_punch", "bind_kick",
    "bind_guard_punch", "bind_punch_kick", "bind_guard_kick", "bind_all_three",
    "bind_start", "bind_coin"
};

struct Settings {
    int aspect = 0;                 // 0 = 3:2 Widescreen, 1 = 4:3 Original, 2 = Stretch
    bool gpu_3d = true;             // true = Mali-G31 GLES 2.0, false = CPU rasterizer
    int render_scale = 1;           // 1 = 1x (496x384 / 558x384), 2 = 2x
    bool arcade_shadows = false;    // false = Smooth blended shadows, true = Checkerboard
    bool smooth_motion = true;      // true = 60fps list interpolation, false = 30Hz arcade 3D
    bool sky_filter = true;         // smooth stage sky panorama
    bool hide_hud = false;
    bool vsync = true;
    bool sync_60hz = false;         // false = 57.524 Hz arcade pacing, true = 60 Hz display sync
    bool show_fps = false;
    int volume = 90;
    int music_volume = 100;
    int effects_volume = 100;
    bool mute = false;
    bool fm_synth = false;          // VF1 YM3438 stays silent; OFF saves Core 2 CPU

    // Arcade & Cheats
    bool free_play = true;
    bool fast_boot = true;
    int secret_char = 0;            // 0..7 (model1::UNUSED_CHARACTERS)
    int dural_colour = 0;           // 0=Chrome, 1=Gold (P2), 2=Gold (Both)
    bool kage_unmasked = false;
    int random_mode = 0;            // 0=Off, 1=P1 Random, 2=CPU Opponents, 3=Both
    int cheat_inf_energy = 0;       // 0=Off, 1=P1, 2=P2, 3=Both
    bool cheat_one_hit = false;
    bool cheat_no_time = false;
    int ring_width = 0;             // 0=Arcade, 1=1500, 2=1400, 3=1300, 4=1200

    arm64::Bindings bindings;

    void defaults() { *this = Settings{}; }

    void sanitize() {
        aspect = std::clamp(aspect, 0, 2);
        render_scale = std::clamp(render_scale, 1, 2);
        volume = std::clamp(volume, 0, 100);
        music_volume = std::clamp(music_volume, 0, 100);
        effects_volume = std::clamp(effects_volume, 0, 100);
        secret_char = std::clamp(secret_char, 0, 7);
        dural_colour = std::clamp(dural_colour, 0, 2);
        random_mode = std::clamp(random_mode, 0, 3);
        cheat_inf_energy = std::clamp(cheat_inf_energy, 0, 3);
        ring_width = std::clamp(ring_width, 0, 4);
        bindings.sanitize();
    }

    void load(const std::string &path) {
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f) return;
        char line[128], key[48], sval[64];
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%47[^=]=%63s", key, sval) != 2) continue;
            const int v = std::atoi(sval);
            if (!std::strcmp(key, "aspect")) aspect = v;
            else if (!std::strcmp(key, "gpu_3d")) gpu_3d = v != 0;
            else if (!std::strcmp(key, "render_scale")) render_scale = v;
            else if (!std::strcmp(key, "arcade_shadows")) arcade_shadows = v != 0;
            else if (!std::strcmp(key, "smooth_motion")) smooth_motion = v != 0;
            else if (!std::strcmp(key, "sky_filter")) sky_filter = v != 0;
            else if (!std::strcmp(key, "hide_hud")) hide_hud = v != 0;
            else if (!std::strcmp(key, "vsync")) vsync = v != 0;
            else if (!std::strcmp(key, "sync_60hz")) sync_60hz = v != 0;
            else if (!std::strcmp(key, "show_fps")) show_fps = v != 0;
            else if (!std::strcmp(key, "volume")) volume = v;
            else if (!std::strcmp(key, "music_volume")) music_volume = v;
            else if (!std::strcmp(key, "effects_volume")) effects_volume = v;
            else if (!std::strcmp(key, "mute")) mute = v != 0;
            else if (!std::strcmp(key, "fm_synth")) fm_synth = v != 0;
            else if (!std::strcmp(key, "free_play")) free_play = v != 0;
            else if (!std::strcmp(key, "fast_boot")) fast_boot = v != 0;
            else if (!std::strcmp(key, "secret_char")) secret_char = v;
            else if (!std::strcmp(key, "dural_colour")) dural_colour = v;
            else if (!std::strcmp(key, "kage_unmasked")) kage_unmasked = v != 0;
            else if (!std::strcmp(key, "random_mode")) random_mode = v;
            else if (!std::strcmp(key, "cheat_inf_energy")) cheat_inf_energy = v;
            else if (!std::strcmp(key, "cheat_one_hit")) cheat_one_hit = v != 0;
            else if (!std::strcmp(key, "cheat_no_time")) cheat_no_time = v != 0;
            else if (!std::strcmp(key, "ring_width")) ring_width = v;
            else {
                for (int a = 0; a < arm64::ActionCount; ++a)
                    if (!std::strcmp(key, kBindKeys[a])) bindings.button[a] = uint32_t(std::strtoul(sval, nullptr, 10));
            }
        }
        std::fclose(f);
        sanitize();
    }

    bool save(const std::string &path) const {
        const std::string tmp = path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "aspect=%d\ngpu_3d=%d\nrender_scale=%d\narcade_shadows=%d\nsmooth_motion=%d\nsky_filter=%d\nhide_hud=%d\nvsync=%d\nsync_60hz=%d\nshow_fps=%d\n",
                     aspect, int(gpu_3d), render_scale, int(arcade_shadows), int(smooth_motion), int(sky_filter), int(hide_hud), int(vsync), int(sync_60hz), int(show_fps));
        std::fprintf(f, "volume=%d\nmusic_volume=%d\neffects_volume=%d\nmute=%d\nfm_synth=%d\n",
                     volume, music_volume, effects_volume, int(mute), int(fm_synth));
        std::fprintf(f, "free_play=%d\nfast_boot=%d\nsecret_char=%d\ndural_colour=%d\nkage_unmasked=%d\nrandom_mode=%d\n",
                     int(free_play), int(fast_boot), secret_char, dural_colour, int(kage_unmasked), random_mode);
        std::fprintf(f, "cheat_inf_energy=%d\ncheat_one_hit=%d\ncheat_no_time=%d\nring_width=%d\n",
                     cheat_inf_energy, int(cheat_one_hit), int(cheat_no_time), ring_width);
        for (int a = 0; a < arm64::ActionCount; ++a)
            std::fprintf(f, "%s=%u\n", kBindKeys[a], unsigned(bindings.button[a]));
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove(tmp.c_str()); return false; }
        std::remove(path.c_str());
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }
};

struct CliOptions {
    std::string data_dir;
    std::string explicit_rom;
    bool fullscreen = true;
    bool show_fps = false;
    bool interp = false;
    bool arcade = false;
    int bench_frames = 0;
    int bench_synthetic = 0;
};

void print_usage(const char *prog) {
    std::printf(
        "ProjectKagemaru — Virtua Fighter (vf1-recomp) ARM64 / OpenGL ES 2.0 Runtime\n"
        "Usage: %s [options]\n"
        "  --data-dir PATH        Data folder containing roms/, saves/, assets/\n"
        "  --rom FILE.zip         Override MAME vf.zip path\n"
        "  --fullscreen           Launch fullscreen (default)\n"
        "  --windowed             Launch in a 720x480 window\n"
        "  --fps                  Enable FPS readout\n"
        "  --interp               Force V60 interpreter instead of SLJIT native code\n"
        "  --arcade               Authentic cabinet boot & coin mode\n"
        "  --bench-frames N       Headless benchmark using vf.zip\n"
        "  --bench-synthetic N    ROM-free headless verification (SLJIT + SoundWorker + 3D export)\n"
        "  --help                 Show this help message\n",
        prog);
}

model1::regions make_synthetic_regions() {
    model1::regions r;
    r.maincpu.assign(0x1000000, 0);
    // NEC V60 reset vector at 0xfffff0: BR8 +0 (0x6a, 0x00) -> tight native loop
    r.maincpu[0xfffff0] = 0x6a;
    r.maincpu[0xfffff1] = 0x00;
    r.copro_tables.assign(0x40000, 0);
    r.polygons.assign(0x10000, 0);
    r.tgp_program.assign(0x2000, 0);
    r.sndcpu.assign(0x100000, 0);
    // M68000 vector table (big-endian): SP=0x00F0FF00, PC=0x00000400
    r.sndcpu[0] = 0x00; r.sndcpu[1] = 0xF0; r.sndcpu[2] = 0xFF; r.sndcpu[3] = 0x00;
    r.sndcpu[4] = 0x00; r.sndcpu[5] = 0x00; r.sndcpu[6] = 0x04; r.sndcpu[7] = 0x00;
    // At 0x400: NOP (0x4E71), BRA.S -4 (0x60FC)
    r.sndcpu[0x400] = 0x4E; r.sndcpu[0x401] = 0x71;
    r.sndcpu[0x402] = 0x60; r.sndcpu[0x403] = 0xFC;
    r.pcm1.assign(0x100000, 0);
    r.pcm2.assign(0x100000, 0);
    return r;
}

int run_synthetic_bench(int frames) {
    model1::regions r = make_synthetic_regions();

    arm64::SoundWorker snd;
    snd.set_core(kSoundCore);
    snd.open(r, nullptr);

    model1 m(std::move(r));
    m.set_drawing(false);

    std::vector<uint8_t> uart;
    m.on_uart_tx = [&](uint8_t b) { uart.push_back(b); };

    rec::hints hints;
    hints.entries.insert(0xfffff0);
    std::vector<uint8_t> mem = m.maincpu_rom();
    mem.resize(0x2000000, 0);
    rec::attach(m.cpu());
    rec::native_stats st;
    const bool jit_ok = rec::build_native(mem, hints, &st);
    if (jit_ok) m.run_cpu = [](int cycles) { rec::run(cycles); };
    m.reset();
    if (jit_ok) rec::attach(m.cpu());

    std::printf("[synthetic] SLJIT native: %zu functions, %zu/%zu insns native, %zu code bytes (%.2f ms)\n",
                st.functions, st.native, st.instructions, st.code_bytes, st.ms);

    const uint64_t t0 = ticks_us();
    for (int f = 0; f < frames; ++f) {
        uart.clear();
        m.run_frame();
        snd.wait();
        snd.submit(std::move(uart));
    }
    snd.wait();
    const double emu_ms = double(ticks_us() - t0) / 1000.0;
    std::printf("[synthetic] Ran %d frames in %.2f ms (%.1f FPS), Core 2 sound samples=%llu\n",
                frames, emu_ms, double(frames) * 1000.0 / std::max(emu_ms, 0.001),
                (unsigned long long)snd.samples_total());
    snd.close();
    if (jit_ok) rec::detach(m.cpu());

    // Verify 3D polygon GPU collector via model1::bench3d
    arm64::GlesPolyCollector col;
    m.set_gpu3d(&col);
    m.set_gpu3d_on(true);
    m.set_gpu3d_target(true);
    m.set_external_gpu(true);
    m.set_smooth(true);
    m.set_parts_mode(true);
    double cpu_ms = 0.0, gpu_ms = 0.0;
    m.bench3d(1, cpu_ms, gpu_ms);
    // Also exercise draw_target directly with 1500 polys
    std::vector<gpu3d_poly> test_polys(1500);
    for (size_t i = 0; i < test_polys.size(); ++i) {
        test_polys[i].x[0] = 10; test_polys[i].y[0] = 10;
        test_polys[i].x[1] = 20; test_polys[i].y[1] = 10;
        test_polys[i].x[2] = 20; test_polys[i].y[2] = 20;
        test_polys[i].x[3] = 10; test_polys[i].y[3] = 20;
        test_polys[i].cx1 = 495; test_polys[i].cy1 = 383;
        test_polys[i].colour = 0x7f7f7f;
    }
    col.draw_target(test_polys.data(), test_polys.size(), nullptr, 0, 0, 496, 384, 0, 383, 0);
    std::printf("[synthetic] 3D export check: %zu gpu3d_poly records ready for GLES 2.0\n",
                col.polys().size());
    return (jit_ok && st.native > 0 && snd.samples_total() > 0 && col.polys().size() >= 1500) ? 0 : 1;
}

std::string find_rom(const std::string &data_dir, const std::string &explicit_rom) {
    if (!explicit_rom.empty()) return explicit_rom;
    const std::string candidates[] = {
        data_dir + "/roms/vf.zip",
        data_dir + "/vf.zip",
        "roms/vf.zip",
    };
    for (const auto &c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) return c;
    }
    return "";
}

std::string find_map_file(const std::string &data_dir) {
    const std::string candidates[] = {
        data_dir + "/assets/vf_map.txt",
        data_dir + "/vf_map.txt",
        "assets/vf_map.txt",
    };
    for (const auto &c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) return c;
    }
    return "";
}

std::vector<std::string> rom_zips_for(const std::string &vf_zip) {
    std::vector<std::string> zips{vf_zip};
    const std::filesystem::path io_zip = std::filesystem::path(vf_zip).parent_path() / "model1io.zip";
    std::error_code ec;
    if (std::filesystem::is_regular_file(io_zip, ec)) zips.push_back(io_zip.string());
    return zips;
}

int run_rom_bench(const std::string &rom_path, const std::string &data_dir, int frames, bool interp) {
    romset::result roms = romset::load(rom_zips_for(rom_path));
    if (!roms.ok) {
        std::fprintf(stderr, "[bench] ROM load error: %s\n", roms.error.c_str());
        return 1;
    }
    arm64::SoundWorker snd;
    snd.set_core(kSoundCore);
    snd.open(roms.regions, nullptr);

    model1 m(std::move(roms.regions));
    arm64::GlesPolyCollector col;
    m.set_gpu3d(&col);
    m.set_gpu3d_on(true);
    m.set_gpu3d_target(true);
    m.set_external_gpu(true);
    m.set_smooth(true);
    m.set_parts_mode(true);
    std::vector<uint8_t> uart;
    m.on_uart_tx = [&](uint8_t b) { uart.push_back(b); };

    rec::native_stats st{};
    const std::string map_path = find_map_file(data_dir);
    bool jit_ok = false;
    if (!interp && !map_path.empty()) {
        rec::hints hints;
        std::vector<uint8_t> mem = m.maincpu_rom();
        mem.resize(0x2000000, 0);
        rec::attach(m.cpu());
        jit_ok = hints.read(map_path) && rec::build_native(mem, hints, &st);
        if (jit_ok) m.run_cpu = [](int cycles) { rec::run(cycles); };
        else rec::detach(m.cpu());
    }
    m.reset();
    if (jit_ok) rec::attach(m.cpu());

    std::printf("[bench] SLJIT: %zu funcs, %zu/%zu native (%.1f ms)\n",
                st.functions, st.native, st.instructions, st.ms);

    const uint64_t t0 = ticks_us();
    uint64_t emu_us = 0, snd_us = 0, ren_us = 0;
    size_t total_polys = 0;
    model1::gpu_parts parts;
    for (int f = 0; f < frames; ++f) {
        const uint64_t a = ticks_us();
        uart.clear();
        m.run_frame();
        const uint64_t b = ticks_us();
        snd.wait();
        snd.submit(std::move(uart));
        const uint64_t c = ticks_us();
        m.render_parts(double(m.frame_number()), parts, true);
        total_polys += col.polys().size();
        const uint64_t d = ticks_us();
        emu_us += (b - a);
        snd_us += (c - b);
        ren_us += (d - c);
    }
    snd.wait();
    snd.close();
    if (jit_ok) rec::detach(m.cpu());
    const double ms = double(ticks_us() - t0) / 1000.0;
    std::printf("[bench] %d frames in %.2f ms (%.2f FPS), avg polys=%.1f | emu=%.2fms snd_wait=%.2fms export=%.2fms\n",
                frames, ms, double(frames) * 1000.0 / std::max(ms, 0.001), double(total_polys) / frames,
                double(emu_us) / (1000.0 * frames), double(snd_us) / (1000.0 * frames), double(ren_us) / (1000.0 * frames));
    return 0;
}

int ring_width_from_idx(int idx) {
    static constexpr int widths[] = {0, 1500, 1400, 1300, 1200};
    return (idx >= 0 && idx < 5) ? widths[idx] : 0;
}

void apply_board_settings(model1 &m, const Settings &s, arm64::SoundWorker &snd) {
    const float asp = (s.aspect == 0) ? 1.5f : (4.0f / 3.0f);
    m.set_aspect(asp);
    m.set_render_scale(s.render_scale);
    m.set_arcade_shadows(s.arcade_shadows);
    m.set_arcade_frames(!s.smooth_motion);
    m.set_sky_filter(s.sky_filter);
    m.set_hud_hidden(s.hide_hud);
    m.set_gpu3d_on(s.gpu_3d);
    m.set_external_gpu(s.gpu_3d);
    m.set_dural_colour(s.dural_colour);
    m.set_kage_unmasked(s.kage_unmasked);
    m.set_ring_width(ring_width_from_idx(s.ring_width));
    snd.set_volumes(float(s.music_volume) / 100.0f, float(s.effects_volume) / 100.0f);
    snd.set_fm_enabled(s.fm_synth);
}

void save_nvram(model1 &m, const std::string &saves_dir) {
    ensure_dir(saves_dir);
    save_bytes(saves_dir + "/nvram.sav", m.nvram().data(), m.nvram().size());
    const std::string ee_path = saves_dir + (m.real_ioboard() ? "/eeprom_93c46.sav" : "/eeprom.sav");
    save_bytes(ee_path, m.eeprom(), model1::EEPROM_SIZE);
    save_text(saves_dir + "/ranking_names.txt", m.ranking_names());
}

void load_nvram(model1 &m, const std::string &saves_dir) {
    load_bytes(saves_dir + "/nvram.sav", m.nvram().data(), m.nvram().size());
    const std::string ee_path = saves_dir + (m.real_ioboard() ? "/eeprom_93c46.sav" : "/eeprom.sav");
    load_bytes(ee_path, m.eeprom(), model1::EEPROM_SIZE);
    const std::string rnames = load_text(saves_dir + "/ranking_names.txt");
    if (!rnames.empty()) m.set_ranking_names(rnames);
}

} // namespace

int main(int argc, char **argv) {
    CliOptions cli;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else if (a == "--data-dir" && i + 1 < argc) cli.data_dir = argv[++i];
        else if (a == "--rom" && i + 1 < argc) cli.explicit_rom = argv[++i];
        else if (a == "--fullscreen") cli.fullscreen = true;
        else if (a == "--windowed") cli.fullscreen = false;
        else if (a == "--fps") cli.show_fps = true;
        else if (a == "--interp") cli.interp = true;
        else if (a == "--arcade") cli.arcade = true;
        else if (a == "--bench-frames" && i + 1 < argc) cli.bench_frames = std::max(1, std::atoi(argv[++i]));
        else if (a == "--bench-synthetic" && i + 1 < argc) cli.bench_synthetic = std::max(1, std::atoi(argv[++i]));
    }

    arm64::pin_current_thread(kMainCore);
    arm64::set_current_thread_name("kagemaru-main");
    setenv("VF1_THREADS", "2", 0);
#if defined(__aarch64__)
    setenv("VF1_WORKER_CORE", "3", 0);
#endif

    if (cli.bench_synthetic > 0) return run_synthetic_bench(cli.bench_synthetic);

    std::string data_dir = cli.data_dir.empty() ? executable_dir(argv[0]) : cli.data_dir;
    if (cli.bench_frames > 0) {
        const std::string rp = find_rom(data_dir, cli.explicit_rom);
        if (rp.empty()) { std::fprintf(stderr, "vf.zip not found in %s/roms/\n", data_dir.c_str()); return 1; }
        return run_rom_bench(rp, data_dir, cli.bench_frames, cli.interp);
    }

    ensure_dir(data_dir + "/roms");
    ensure_dir(data_dir + "/saves");
    const std::string settings_path = data_dir + "/saves/settings.ini";
    Settings cfg;
    cfg.load(settings_path);
    if (cli.show_fps) cfg.show_fps = true;
    if (cli.arcade) { cfg.free_play = false; cfg.fast_boot = false; }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    arm64::GlConfig gl_cfg;
    gl_cfg.fullscreen = cli.fullscreen;
    gl_cfg.vsync = cfg.vsync;
    if (!arm64::gl_init(gl_cfg)) {
        std::fprintf(stderr, "OpenGL init failed: %s\n", arm64::gl_error());
        SDL_Quit();
        return 1;
    }

    arm64::GpuGlesRenderer gpu;
    arm64::ImGuiGles imgui;
    if (!gpu.ok() || !imgui.init()) {
        std::fprintf(stderr, "Renderer / ImGui init failed\n");
        imgui.shutdown();
        gpu.shutdown();
        arm64::gl_fini();
        SDL_Quit();
        return 1;
    }

    arm64::PadManager pads;
    pads.init(data_dir);

    const std::string rom_path = find_rom(data_dir, cli.explicit_rom);
    romset::result roms;
    std::string rom_error;
    if (rom_path.empty()) {
        rom_error = "Missing ROM archive: place MAME vf.zip (with 315-5724.bin) in:\n" + data_dir + "/roms/vf.zip";
    } else {
        roms = romset::load(rom_zips_for(rom_path));
        if (!roms.ok) rom_error = "Invalid vf.zip:\n" + roms.error;
    }

    if (!rom_error.empty()) {
        bool quit = false;
        while (!quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) quit = true;
                else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN)) quit = true;
                else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) arm64::gl_update_drawable_size();
                else pads.handle_event(e);
            }
            const uint32_t b0 = pads.poll_player(0, SDL_GetKeyboardState(nullptr));
            if (b0 & (arm64::BtnA | arm64::BtnB | arm64::Start | arm64::Guide)) quit = true;
            arm64::gl_begin_frame();
            imgui.frame(1.0f / 60.0f);
            ImGui::SetNextWindowPos({float(arm64::gl_display_width()) * 0.5f, float(arm64::gl_display_height()) * 0.5f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({std::min(640.0f, float(arm64::gl_display_width()) - 40.0f), 0.0f});
            ImGui::Begin("ProjectKagemaru — ROM Required", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
            ImGui::TextWrapped("%s", rom_error.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Zero ROMs are bundled. Supply your own MAME vf.zip.");
            ImGui::Spacing();
            if (ImGui::Button("Exit (Press Start / A / Esc)", {-1.0f, 0.0f})) quit = true;
            ImGui::End();
            imgui.render();
            arm64::gl_end_frame();
            SDL_Delay(16);
        }
        imgui.shutdown();
        gpu.shutdown();
        arm64::gl_fini();
        SDL_Quit();
        return 1;
    }

    arm64::Audio audio;
    const bool audio_ok = audio.open(m1audio::OUT_RATE);
    if (audio_ok) {
        audio.volume(float(cfg.volume) / 100.0f);
        audio.mute(cfg.mute);
    }

    arm64::SoundWorker sound_worker;
    sound_worker.set_core(kSoundCore);
    sound_worker.open(roms.regions, audio_ok ? &audio : nullptr);

    std::unique_ptr<model1> game = std::make_unique<model1>(std::move(roms.regions));
    arm64::GlesPolyCollector collector;
    arm64::GlesPolyCollector *poly_col = &collector;
    game->set_gpu3d(poly_col);
    game->set_gpu3d_target(true);
    game->set_smooth(true);
    game->set_parts_mode(true);

    std::vector<uint8_t> uart_bytes;
    game->on_uart_tx = [&](uint8_t b) { uart_bytes.push_back(b); };

    load_backgrounds(*game, data_dir + "/assets/backgrounds");
    load_nvram(*game, data_dir + "/saves");

    rec::hints v60_map;
    const std::string map_path = find_map_file(data_dir);
    bool have_map = !cli.interp && !map_path.empty() && v60_map.read(map_path);
    if (!cli.interp && !have_map) {
        std::fprintf(stderr, "[ProjectKagemaru] WARNING: vf_map.txt not found; running V60 interpreter\n");
    }

    rec::native_stats native_stats{};
    bool jit_active = false;
    bool jit_dirty = true;
    bool last_no_time = cfg.cheat_no_time;
    bool last_strands = cfg.random_mode > 0;

    auto compile_v60 = [&]() {
        if (!have_map) return;
        std::vector<uint8_t> mem = game->maincpu_rom();
        mem.resize(0x2000000, 0);
        rec::attach(game->cpu());
        if (rec::build_native(mem, v60_map, &native_stats)) {
            game->run_cpu = [](int cycles) { rec::run(cycles); };
            jit_active = true;
            std::fprintf(stderr, "[ProjectKagemaru] SLJIT V60: %zu funcs, %zu/%zu insns native, %zu KB in %.1f ms\n",
                         native_stats.functions, native_stats.native, native_stats.instructions,
                         native_stats.code_bytes / 1024, native_stats.ms);
        } else {
            rec::detach(game->cpu());
            game->run_cpu = nullptr;
            jit_active = false;
        }
        jit_dirty = false;
    };

    EmuWorker emu_worker;

    auto restart_game = [&](bool skip_boot) {
        emu_worker.mark_render_done();
        emu_worker.wait_frame();
        sound_worker.set_paused(true);
        sound_worker.reset();

        if (cfg.cheat_no_time != last_no_time) {
            if (game->set_no_time_limit(cfg.cheat_no_time)) jit_dirty = true;
            last_no_time = cfg.cheat_no_time;
        } else {
            game->set_no_time_limit(cfg.cheat_no_time);
        }
        const bool want_strands = cfg.random_mode > 0;
        if (want_strands != last_strands) {
            if (game->set_strands_moved(want_strands)) jit_dirty = true;
            last_strands = want_strands;
        } else {
            game->set_strands_moved(want_strands);
        }

        if (jit_dirty) compile_v60();

        if (cfg.free_play) {
            game->force_free_play();
            game->hide_free_play_text();
        } else {
            game->show_free_play_text();
        }

        game->set_akira_slot(cfg.secret_char);
        game->set_random_player(0, (cfg.random_mode == 1 || cfg.random_mode == 3) ? "*" : "");
        game->set_random_player(1, "");
        game->set_random_fighter(cfg.random_mode > 0, false, cfg.random_mode == 2 || cfg.random_mode == 3);

        game->reset();
        if (jit_active) rec::attach(game->cpu());
        std::fill_n(&game->work_ram()[0x17f0], 4, 0);
        apply_board_settings(*game, cfg, sound_worker);
        gpu.reset_caches();
        poly_col->clear();

        if (skip_boot && cfg.fast_boot) {
            game->set_drawing(false);
            for (int f = 0; f < 900 && !game->past_boot(); ++f) {
                uart_bytes.clear();
                game->run_frame();
                sound_worker.wait();
                sound_worker.submit(std::move(uart_bytes));
            }
            sound_worker.wait();
            game->set_drawing(true);
        }
        sound_worker.set_paused(false);
    };

    restart_game(true);
    emu_worker.open(game.get(), kEmuCore);

    enum class MenuPage { Main, Display, Audio, Arcade, Cheats, Controls };
    bool running = true;
    bool menu_open = false;
    MenuPage menu_page = MenuPage::Main;
    int menu_idx = 0;
    bool menu_latch = false;
    int test_hold_frames = 0;
    bool test_hold_armed = false;
    uint32_t prev_p0_menu = 0;
    uint32_t prev_rank_ver = game->ranking_names_version();

    model1::gpu_parts parts;
    int last_parts_rc = 0;
    uint64_t next_frame_us = ticks_us();
    uint64_t fps_t0 = next_frame_us;
    uint64_t log_t0 = next_frame_us;
    int fps_frames = 0;
    int log_frames = 0;
    double current_fps = 57.524;
    double last_emu_ms = 0.0, last_parts_ms = 0.0, last_gles_ms = 0.0, last_swap_ms = 0.0;
    double sum_emu_ms = 0.0, sum_snd_ms = 0.0, sum_parts_ms = 0.0, sum_gles_ms = 0.0, sum_swap_ms = 0.0;
    double sum_polys = 0.0, sum_batches = 0.0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                arm64::gl_update_drawable_size();
            else pads.handle_event(e);
        }

        const uint8_t *keys = SDL_GetKeyboardState(nullptr);
        const uint32_t p0_btns = pads.poll_player(0, keys);
        const uint32_t p1_btns = pads.poll_player(1, keys);

        const bool chord_now = arm64::menu_chord(p0_btns);
        if (chord_now && !menu_latch) {
            menu_open = !menu_open;
            menu_page = MenuPage::Main;
            menu_idx = 0;
            if (menu_open) {
                emu_worker.mark_render_done();
                emu_worker.wait_frame();
                sound_worker.set_paused(true);
                if (audio_ok) audio.pause();
            } else {
                sound_worker.set_paused(false);
                cfg.save(settings_path);
            }
            next_frame_us = ticks_us();
        }
        menu_latch = chord_now;

        if (!menu_open) {
            uint8_t in0 = 0xff, in1 = 0xff, in2 = 0xff;
            const bool sys_combo = (p0_btns & arm64::Select) != 0;
            if (sys_combo && (p0_btns & arm64::BtnY)) in0 &= ~arm64::IN0_TEST;
            if (sys_combo && (p0_btns & arm64::BtnX)) in0 &= ~arm64::IN0_SERVICE;
            if (test_hold_frames > 0) {
                in0 &= ~arm64::IN0_TEST;
                --test_hold_frames;
            }
            arm64::apply_player_buttons(p0_btns, cfg.bindings, false, in0, in1, sys_combo);
            arm64::apply_player_buttons(p1_btns, cfg.bindings, true, in0, in2, false);

            if (!emu_worker.is_busy()) {
                game->m_inputs[0] = in0;
                game->m_inputs[1] = in1;
                game->m_inputs[2] = in2;
                game->fight_cheats(cfg.cheat_inf_energy, cfg.cheat_one_hit, cfg.cheat_no_time);
                uart_bytes.clear();
                emu_worker.start_frame(false);
            }

            emu_worker.wait_frame();
            last_emu_ms = emu_worker.last_emu_ms();

            if (!uart_bytes.empty()) {
                sound_worker.push_uart(uart_bytes.data(), uart_bytes.size());
                uart_bytes.clear();
            }

            if (game->ranking_names_version() != prev_rank_ver) {
                prev_rank_ver = game->ranking_names_version();
                save_text(data_dir + "/saves/ranking_names.txt", game->ranking_names());
            }

            const uint64_t t_ren0 = ticks_us();
            const arm64::AspectMode aspect_mode = static_cast<arm64::AspectMode>(cfg.aspect);
            const int rc = game->render_parts(double(game->frame_number()), parts, true);
            if (rc != 0) last_parts_rc = rc;
            const int game_w = game->arcade_width();
            const uint64_t t_parts1 = ticks_us();
            last_parts_ms = double(t_parts1 - t_ren0) / 1000.0;

            const bool use_parts = (last_parts_rc == 1);
            // Launch Core 1 on NEXT frame in parallel with Core 0 GLES + Swap
            game->m_inputs[0] = in0;
            game->m_inputs[1] = in1;
            game->m_inputs[2] = in2;
            game->fight_cheats(cfg.cheat_inf_energy, cfg.cheat_one_hit, cfg.cheat_no_time);
            emu_worker.start_frame(use_parts);

            arm64::gl_begin_frame();
            if (use_parts) {
                const auto &polys = poly_col->polys();
                gpu.draw_frame(parts, game_w, aspect_mode,
                               polys.data(), polys.size(),
                               cfg.arcade_shadows, cfg.render_scale);
                emu_worker.mark_render_done();
            } else {
                const bitmap_rgb32 &fb = game->frame();
                gpu.draw_cpu_framebuffer(fb.data(), fb.width(), fb.height(), aspect_mode);
            }
            last_gles_ms = double(ticks_us() - t_parts1) / 1000.0;

            if (cfg.show_fps) {
                imgui.frame(1.0f / 60.0f);
                ImGui::SetNextWindowPos({8.0f, 8.0f}, ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.65f);
                ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
                ImGui::Text("FPS: %.1f | Emu:%.1f 2D:%.1f GL:%.1f Swap:%.1f Snd:%.1f | P:%zu(%zu)",
                            current_fps, last_emu_ms, last_parts_ms, last_gles_ms, last_swap_ms,
                            sound_worker.last_work_ms(), gpu.last_poly_count(), gpu.last_batch_count());
                ImGui::End();
                imgui.render();
            }
            const uint64_t t_swap0 = ticks_us();
            arm64::gl_end_frame();
            const uint64_t now = ticks_us();
            last_swap_ms = double(now - t_swap0) / 1000.0;

            ++fps_frames;
            ++log_frames;
            sum_emu_ms += last_emu_ms;
            sum_snd_ms += sound_worker.last_work_ms();
            sum_parts_ms += last_parts_ms;
            sum_gles_ms += last_gles_ms;
            sum_swap_ms += last_swap_ms;
            sum_polys += double(gpu.last_poly_count());
            sum_batches += double(gpu.last_batch_count());

            if (now - fps_t0 >= 500000ull) {
                current_fps = double(fps_frames) * 1000000.0 / double(now - fps_t0);
                fps_frames = 0;
                fps_t0 = now;
            }
            if (now - log_t0 >= 5000000ull && log_frames > 0) {
                const double lf = double(log_frames) * 1000000.0 / double(now - log_t0);
                std::fprintf(stderr,
                             "[perf] fps=%.1f emu=%.2fms snd=%.2fms 2d_exp=%.2fms gles=%.2fms swap=%.2fms polys=%.0f batches=%.0f\n",
                             lf, sum_emu_ms / log_frames, sum_snd_ms / log_frames, sum_parts_ms / log_frames,
                             sum_gles_ms / log_frames, sum_swap_ms / log_frames, sum_polys / log_frames, sum_batches / log_frames);
                log_frames = 0;
                log_t0 = now;
                sum_emu_ms = sum_snd_ms = sum_parts_ms = sum_gles_ms = sum_swap_ms = sum_polys = sum_batches = 0.0;
            }
            if (!cfg.vsync) {
                const uint64_t period_us = cfg.sync_60hz ? 16667ull : 17384ull;
                next_frame_us += period_us;
                if (now < next_frame_us) {
                    const uint64_t wait_ms = (next_frame_us - now) / 1000ull;
                    if (wait_ms > 1) SDL_Delay(Uint32(wait_ms - 1));
                } else if (now - next_frame_us > period_us * 4) {
                    next_frame_us = now;
                }
            } else {
                next_frame_us = now;
            }
        } else {
            const uint32_t edge = p0_btns & ~prev_p0_menu;
            prev_p0_menu = p0_btns;
            auto go_back = [&]() {
                if (menu_page != MenuPage::Main) { menu_page = MenuPage::Main; menu_idx = 0; }
                else { menu_open = false; sound_worker.set_paused(false); cfg.save(settings_path); next_frame_us = ticks_us(); }
            };
            if (edge & arm64::BtnB) go_back();

            const int item_counts[] = {8, 9, 5, 6, 9, arm64::ActionCount + 2};
            const int n_items = item_counts[int(menu_page)];
            if (edge & arm64::Up) menu_idx = (menu_idx + n_items - 1) % n_items;
            if (edge & arm64::Down) menu_idx = (menu_idx + 1) % n_items;
            const int lr = (edge & arm64::Right) ? 1 : ((edge & arm64::Left) ? -1 : 0);
            const bool act = (edge & (arm64::BtnA | arm64::Start)) != 0;

            arm64::gl_begin_frame();
            imgui.frame(1.0f / 60.0f);
            ImGui::SetNextWindowPos({float(arm64::gl_display_width()) * 0.5f, float(arm64::gl_display_height()) * 0.5f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({std::min(560.0f, float(arm64::gl_display_width()) - 24.0f), 0.0f});
            const char *titles[] = {
                "ProjectKagemaru — Virtua Fighter", "Display Settings", "Audio Settings",
                "Arcade Settings", "Cheats & Secrets", "Controls (P1)"
            };
            ImGui::Begin(titles[int(menu_page)], nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
            ImGui::TextDisabled("%s | SLJIT: %zu insns", arm64::gl_renderer_string(), native_stats.native);
            ImGui::Separator();

            auto row = [&](int idx, const char *label, const char *val) -> bool {
                const bool sel = (menu_idx == idx);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.85f, 0.24f, 0.32f, 0.90f));
                const bool clicked = ImGui::Selectable(label, sel);
                if (sel) ImGui::PopStyleColor();
                if (val && *val) {
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 160.0f);
                    ImGui::Text("%s", val);
                }
                return clicked || (sel && (act || lr != 0));
            };

            if (menu_page == MenuPage::Main) {
                if (row(0, "Resume Game", nullptr) && (act || ImGui::IsItemClicked())) {
                    menu_open = false; sound_worker.set_paused(false); cfg.save(settings_path); next_frame_us = ticks_us();
                }
                if (row(1, "Display", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Display; menu_idx = 0; }
                if (row(2, "Audio", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Audio; menu_idx = 0; }
                if (row(3, "Arcade Settings", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Arcade; menu_idx = 0; }
                if (row(4, "Cheats & Secrets", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Cheats; menu_idx = 0; }
                if (row(5, "Controls", "->") && (act || lr > 0 || ImGui::IsItemClicked())) { menu_page = MenuPage::Controls; menu_idx = 0; }
                if (row(6, "Reset Machine", nullptr) && (act || ImGui::IsItemClicked())) {
                    restart_game(true); menu_open = false; next_frame_us = ticks_us();
                }
                if (row(7, "Quit ProjectKagemaru", nullptr) && (act || ImGui::IsItemClicked())) running = false;
            } else if (menu_page == MenuPage::Display) {
                const char *asp_names[] = {"3:2 Widescreen", "4:3 Original", "Stretch Fill"};
                if (row(0, "Aspect Ratio", asp_names[cfg.aspect])) {
                    cfg.aspect = (cfg.aspect + (lr < 0 ? 2 : 1)) % 3;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(1, "3D Renderer", cfg.gpu_3d ? "GLES 2.0 (Mali)" : "CPU Software")) {
                    cfg.gpu_3d = !cfg.gpu_3d;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(2, "Internal Scale", cfg.render_scale == 2 ? "2x (HD)" : "1x (Arcade)")) {
                    cfg.render_scale = (cfg.render_scale == 1) ? 2 : 1;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(3, "Shadows", cfg.arcade_shadows ? "Arcade Checker" : "Smooth Blended")) {
                    cfg.arcade_shadows = !cfg.arcade_shadows;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(4, "Smooth Motion (60fps 3D)", cfg.smooth_motion ? "ON" : "OFF (30Hz)")) {
                    cfg.smooth_motion = !cfg.smooth_motion;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(5, "Sky Filter", cfg.sky_filter ? "Smooth" : "Sharp")) {
                    cfg.sky_filter = !cfg.sky_filter;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(6, "Hide HUD (Fight)", cfg.hide_hud ? "ON" : "OFF")) {
                    cfg.hide_hud = !cfg.hide_hud;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(7, "VSync", cfg.vsync ? "ON" : "OFF")) {
                    cfg.vsync = !cfg.vsync;
                    arm64::gl_set_vsync(cfg.vsync);
                }
                if (row(8, "Show FPS / Stats", cfg.show_fps ? "ON" : "OFF")) cfg.show_fps = !cfg.show_fps;
            } else if (menu_page == MenuPage::Audio) {
                char vbuf[32];
                std::snprintf(vbuf, sizeof vbuf, "%d%%", cfg.volume);
                if (row(0, "Master Volume", vbuf)) {
                    cfg.volume = std::clamp(cfg.volume + (lr ? lr * 5 : 10), 0, 100);
                    if (audio_ok) audio.volume(float(cfg.volume) / 100.0f);
                }
                std::snprintf(vbuf, sizeof vbuf, "%d%%", cfg.music_volume);
                if (row(1, "Music Volume", vbuf)) {
                    cfg.music_volume = std::clamp(cfg.music_volume + (lr ? lr * 5 : 10), 0, 100);
                    apply_board_settings(*game, cfg, sound_worker);
                }
                std::snprintf(vbuf, sizeof vbuf, "%d%%", cfg.effects_volume);
                if (row(2, "SFX / Voice Volume", vbuf)) {
                    cfg.effects_volume = std::clamp(cfg.effects_volume + (lr ? lr * 5 : 10), 0, 100);
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(3, "Mute Audio", cfg.mute ? "ON" : "OFF")) {
                    cfg.mute = !cfg.mute;
                    if (audio_ok) audio.mute(cfg.mute);
                }
                if (row(4, "YM3438 FM Synth", cfg.fm_synth ? "ON" : "OFF (Fast)")) {
                    cfg.fm_synth = !cfg.fm_synth;
                    apply_board_settings(*game, cfg, sound_worker);
                }
            } else if (menu_page == MenuPage::Arcade) {
                if (row(0, "Coin Mode", cfg.free_play ? "Free Play" : "Coin / Cabinet")) {
                    cfg.free_play = !cfg.free_play;
                    restart_game(true);
                }
                const int diff = std::clamp(game->assignment(model1::game_assignment::difficulty), 0, 3);
                const char *dnames[] = {"Easy", "Normal", "Hard", "Hardest"};
                if (row(1, "Difficulty", dnames[diff])) {
                    game->set_assignment(model1::game_assignment::difficulty, (diff + (lr < 0 ? 3 : 1)) % 4);
                }
                const int r1p = std::clamp(game->assignment(model1::game_assignment::match_1p), 2, 5);
                char rbuf[16]; std::snprintf(rbuf, sizeof rbuf, "%d", r1p);
                if (row(2, "Rounds to Win (1P)", rbuf)) {
                    const int next_r = (r1p >= 5) ? 2 : (r1p + 1);
                    game->set_assignment(model1::game_assignment::match_1p, next_r);
                }
                const int lang = std::clamp(game->assignment(model1::game_assignment::language), 0, 1);
                if (row(3, "Cabinet Language", lang ? "English" : "Japanese")) {
                    game->set_assignment(model1::game_assignment::language, lang ^ 1);
                }
                if (row(4, "Hold Test Button (Next Resume)", test_hold_armed ? "ARMED" : "OFF")) {
                    test_hold_armed = !test_hold_armed;
                    test_hold_frames = test_hold_armed ? 30 : 0;
                }
                if (row(5, "Apply & Restart Machine", nullptr) && (act || ImGui::IsItemClicked())) {
                    restart_game(true); menu_open = false; next_frame_us = ticks_us();
                }
            } else if (menu_page == MenuPage::Cheats) {
                if (row(0, "Secret Character (Akira Slot)", model1::UNUSED_CHARACTERS[cfg.secret_char])) {
                    cfg.secret_char = (cfg.secret_char + (lr < 0 ? 7 : 1)) % 8;
                }
                const char *dcols[] = {"Chrome", "Gold (P2)", "Gold (Both)"};
                if (row(1, "Dural Colour", dcols[cfg.dural_colour])) {
                    cfg.dural_colour = (cfg.dural_colour + (lr < 0 ? 2 : 1)) % 3;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(2, "Kage Unmasked", cfg.kage_unmasked ? "ON" : "OFF")) {
                    cfg.kage_unmasked = !cfg.kage_unmasked;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                const char *rmodes[] = {"Off", "P1 Random", "CPU Opponents", "Both"};
                if (row(3, "Random Fighters", rmodes[cfg.random_mode])) {
                    cfg.random_mode = (cfg.random_mode + (lr < 0 ? 3 : 1)) % 4;
                }
                const char *infs[] = {"Off", "Player 1", "Player 2", "Both"};
                if (row(4, "Infinite Energy", infs[cfg.cheat_inf_energy])) {
                    cfg.cheat_inf_energy = (cfg.cheat_inf_energy + (lr < 0 ? 3 : 1)) % 4;
                }
                if (row(5, "One-Hit KO", cfg.cheat_one_hit ? "ON" : "OFF")) cfg.cheat_one_hit = !cfg.cheat_one_hit;
                if (row(6, "No Time Limit", cfg.cheat_no_time ? "ON" : "OFF")) cfg.cheat_no_time = !cfg.cheat_no_time;
                const char *rings[] = {"Arcade", "1500 (Wide)", "1400", "1300", "1200 (Narrow)"};
                if (row(7, "Ring Width", rings[cfg.ring_width])) {
                    cfg.ring_width = (cfg.ring_width + (lr < 0 ? 4 : 1)) % 5;
                    apply_board_settings(*game, cfg, sound_worker);
                }
                if (row(8, "Apply & Restart (Secrets/Random)", nullptr) && (act || ImGui::IsItemClicked())) {
                    restart_game(true); menu_open = false; next_frame_us = ticks_us();
                }
            } else if (menu_page == MenuPage::Controls) {
                for (int a = 0; a < arm64::ActionCount; ++a) {
                    if (row(a, arm64::action_name(a), arm64::button_name(cfg.bindings.button[a]))) {
                        cfg.bindings.cycle(a, lr < 0 ? -1 : 1);
                    }
                }
                if (row(arm64::ActionCount, "Reset Default Controls", nullptr) && (act || ImGui::IsItemClicked())) {
                    cfg.bindings = arm64::Bindings{};
                }
                if (row(arm64::ActionCount + 1, "Back", nullptr) && (act || ImGui::IsItemClicked())) {
                    menu_page = MenuPage::Main; menu_idx = 0;
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("D-Pad: Navigate  |  A: Select/Toggle  |  B: Back  |  Start+Select: Close");
            ImGui::End();
            imgui.render();
            arm64::gl_end_frame();
            SDL_Delay(16);
        }
    }

    emu_worker.mark_render_done();
    emu_worker.close();
    save_nvram(*game, data_dir + "/saves");
    cfg.save(settings_path);
    sound_worker.wait();
    sound_worker.close();
    if (audio_ok) audio.close();
    if (jit_active) rec::detach(game->cpu());
    imgui.shutdown();
    gpu.shutdown();
    arm64::gl_fini();
    SDL_Quit();
    return 0;
}
