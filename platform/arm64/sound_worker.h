// Decoupled Core 2 sound board worker for ProjectKagemaru.
// Runs VF1's 68000 (10 MHz) + dual MultiPCM + optional YM3438 in parallel on Core 2,
// copying ONLY sndcpu (512 KB), pcm1 (4 MB), and pcm2 (4 MB) (~8.5 MB RAM).
#pragma once

#include "audio.h"
#include "machine/m1audio.h"
#include "machine/model1.h"
#include "thread_affinity.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace arm64 {

class SoundWorker {
public:
    SoundWorker() = default;
    SoundWorker(const SoundWorker &) = delete;
    SoundWorker &operator=(const SoundWorker &) = delete;
    ~SoundWorker() { close(); }

    void set_core(int core) { core_ = core; }

    bool open(const model1::regions &r, Audio *audio = nullptr) {
        close();
        audio_ = audio;
        sndcpu_ = r.sndcpu;
        pcm1_ = r.pcm1;
        pcm2_ = r.pcm2;
        snd_ = std::make_unique<m1audio>(sndcpu_, pcm1_, pcm2_);
        snd_->reset();
        snd_->set_volumes(music_vol_, sfx_vol_);
        snd_->set_fm_enabled(fm_enabled_);
        stop_ = false;
        busy_ = false;
        in_flight_ = false;
        paused_ = false;
        pending_vol_ = false;
        pending_fm_ = false;
        discard_out_ = false;
        midi_status_ = 0;
        midi_owed_ = 0;
        midi_held_ = 0;
        queued_bytes_.clear();
        pending_midi_.clear();
        audio_out_.clear();
        thread_ = std::thread(&SoundWorker::loop, this);
        return true;
    }

    void push_uart(const uint8_t *data, size_t n) {
        if (!snd_ || !data || !n) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            queued_bytes_.insert(queued_bytes_.end(), data, data + n);
        }
        cv_.notify_one();
    }

    void set_paused(bool paused) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            paused_ = paused;
        }
        cv_.notify_one();
    }

    void submit(std::vector<uint8_t> uart_bytes, bool discard_output = false) {
        if (!snd_) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!uart_bytes.empty()) {
                queued_bytes_.insert(queued_bytes_.end(), uart_bytes.begin(), uart_bytes.end());
            }
            discard_out_ = discard_output;
            busy_ = true;
        }
        cv_.notify_one();
    }

    void wait() {
        if (!thread_.joinable()) return;
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [&] { return !busy_ && !in_flight_; });
    }

    void reset() {
        if (!snd_) return;
        std::unique_lock<std::mutex> lk(mu_);
        busy_ = false;
        discard_out_ = false;
        done_.wait(lk, [&] { return !in_flight_; });
        queued_bytes_.clear();
        pending_midi_.clear();
        midi_status_ = 0;
        midi_owed_ = 0;
        midi_held_ = 0;
        audio_out_.clear();
        snd_->reset();
        snd_->set_volumes(music_vol_, sfx_vol_);
        snd_->set_fm_enabled(fm_enabled_);
        if (audio_) audio_->pause();
    }

    void set_volumes(float music, float effects) {
        music_vol_ = music;
        sfx_vol_ = effects;
        if (!snd_) return;
        std::lock_guard<std::mutex> lk(mu_);
        pending_vol_ = true;
    }

    void set_fm_enabled(bool on) {
        fm_enabled_ = on;
        if (!snd_) return;
        std::lock_guard<std::mutex> lk(mu_);
        pending_fm_ = true;
    }

    uint64_t samples_total() const { return samples_total_.load(std::memory_order_relaxed); }
    double last_work_ms() const { return last_work_ms_.load(std::memory_order_relaxed); }

    void close() {
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                stop_ = true;
            }
            cv_.notify_all();
            thread_.join();
        }
        snd_.reset();
        sndcpu_.clear();
        pcm1_.clear();
        pcm2_.clear();
        audio_out_.clear();
        audio_ = nullptr;
    }

private:
    static constexpr int kLines = 424;
    static constexpr int kByteLines = 8;
    static constexpr double kLineUsec = 656.0 * 1e6 / 16000000.0; // 41.0 us per scanline (17384 us / 424)

    static int midi_data_bytes(uint8_t st) {
        const int k = st >> 4;
        return (k == 0xc || k == 0xd) ? 1 : (k >= 0x8 && k <= 0xe) ? 2 : 0;
    }

    void loop() {
        pin_current_thread(core_);
        set_current_thread_name("kagemaru-snd");
        for (;;) {
            std::vector<uint8_t> incoming;
            bool do_vol = false, do_fm = false, discard_out = false;
            float mvol = 1.0f, svol = 1.0f;
            bool fm = true;
            {
                std::unique_lock<std::mutex> lk(mu_);
                if (audio_) {
                    for (;;) {
                        if (stop_) return;
                        const bool need_audio = !paused_ && (audio_->queued_frames() < AudioRate::kTargetFrames + 768);
                        if (busy_ || need_audio) break;
                        cv_.wait_for(lk, std::chrono::milliseconds(2));
                    }
                } else {
                    cv_.wait(lk, [&] { return stop_ || busy_; });
                    if (stop_) return;
                }
                in_flight_ = true;
                incoming.swap(queued_bytes_);
                discard_out = discard_out_;
                discard_out_ = false;
                do_vol = pending_vol_; pending_vol_ = false;
                do_fm = pending_fm_; pending_fm_ = false;
                mvol = music_vol_; svol = sfx_vol_;
                fm = fm_enabled_;
            }
            const auto t0 = std::chrono::steady_clock::now();
            if (do_vol) snd_->set_volumes(mvol, svol);
            if (do_fm) snd_->set_fm_enabled(fm);
            if (!incoming.empty()) {
                pending_midi_.insert(pending_midi_.end(), incoming.begin(), incoming.end());
            }

            size_t whole = 0;
            {
                uint8_t st = midi_status_;
                int need = midi_owed_;
                for (size_t i = 0; i < pending_midi_.size(); ++i) {
                    const uint8_t b = pending_midi_[i];
                    if (b >= 0xf8) {
                        if (!need) whole = i + 1;
                        continue;
                    }
                    if (b & 0x80) {
                        st = b;
                        need = midi_data_bytes(st);
                        if (!need) whole = i + 1;
                        continue;
                    }
                    if (!need) need = midi_data_bytes(st);
                    if (!need || --need == 0) whole = i + 1;
                }
                midi_held_ = (whole == 0 && !pending_midi_.empty()) ? (midi_held_ + 1) : 0;
                if (midi_held_ > 3 * 58) whole = pending_midi_.size();
            }

            audio_out_.clear();
            size_t sent = 0;
            for (int line = 0; line < kLines; ++line) {
                if ((line % kByteLines) == 0 && sent < whole) {
                    const uint8_t x = pending_midi_[sent++];
                    if (x >= 0xf8) {
                        // real-time byte
                    } else if (x & 0x80) {
                        midi_status_ = x;
                        midi_owed_ = midi_data_bytes(x);
                    } else if (midi_owed_ || (midi_owed_ = midi_data_bytes(midi_status_))) {
                        --midi_owed_;
                    }
                    snd_->rx_byte(x);
                }
                snd_->run(kLineUsec, audio_out_);
                while (snd_->tx_pending()) snd_->tx_byte();
            }
            if (sent > 0) {
                pending_midi_.erase(pending_midi_.begin(), pending_midi_.begin() + std::ptrdiff_t(sent));
            }

            if (!audio_out_.empty()) {
                samples_total_.fetch_add(audio_out_.size() / 2, std::memory_order_relaxed);
                if (audio_ && !discard_out) audio_->push(audio_out_.data(), audio_out_.size());
                audio_out_.clear();
            }
            const double work_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            last_work_ms_.store(work_ms, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mu_);
                busy_ = false;
                in_flight_ = false;
            }
            done_.notify_all();
        }
    }

    int core_ = 2;
    Audio *audio_ = nullptr;
    std::vector<uint8_t> sndcpu_, pcm1_, pcm2_;
    std::unique_ptr<m1audio> snd_;
    std::vector<int16_t> audio_out_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_, done_;
    std::vector<uint8_t> queued_bytes_;
    std::vector<uint8_t> pending_midi_;
    uint8_t midi_status_ = 0;
    int midi_owed_ = 0;
    int midi_held_ = 0;
    bool stop_ = false;
    bool busy_ = false;
    bool in_flight_ = false;
    bool paused_ = false;
    bool discard_out_ = false;
    bool pending_vol_ = false;
    bool pending_fm_ = false;
    bool fm_enabled_ = true;
    float music_vol_ = 1.0f;
    float sfx_vol_ = 1.0f;
    std::atomic<uint64_t> samples_total_{0};
    std::atomic<double> last_work_ms_{0.0};
};

} // namespace arm64
