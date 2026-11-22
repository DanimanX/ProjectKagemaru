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
        queued_bytes_.clear();
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

    void submit(std::vector<uint8_t> uart_bytes) {
        if (!snd_) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!uart_bytes.empty()) {
                queued_bytes_.insert(queued_bytes_.end(), uart_bytes.begin(), uart_bytes.end());
            }
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
        done_.wait(lk, [&] { return !in_flight_; });
        queued_bytes_.clear();
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
    static constexpr double kFrameUsec = 656.0 * 424.0 * 1e6 / 16000000.0; // 17384 us @ 57.524 Hz
    static constexpr int kSubSlices = 54;                                   // ~322 us/slice (i8251 UART cadence)

    void loop() {
        pin_current_thread(core_);
        set_current_thread_name("kagemaru-snd");
        for (;;) {
            std::vector<uint8_t> bytes;
            bool do_vol = false, do_fm = false;
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
                bytes.swap(queued_bytes_);
                do_vol = pending_vol_; pending_vol_ = false;
                do_fm = pending_fm_; pending_fm_ = false;
                mvol = music_vol_; svol = sfx_vol_;
                fm = fm_enabled_;
            }
            const auto t0 = std::chrono::steady_clock::now();
            if (do_vol) snd_->set_volumes(mvol, svol);
            if (do_fm) snd_->set_fm_enabled(fm);
            for (uint8_t b : bytes) snd_->rx_byte(b);
            audio_out_.clear();
            const double slice_usec = kFrameUsec / double(kSubSlices);
            for (int s = 0; s < kSubSlices; ++s) {
                snd_->run(slice_usec, audio_out_);
            }
            if (!audio_out_.empty()) {
                samples_total_.fetch_add(audio_out_.size() / 2, std::memory_order_relaxed);
                if (audio_) audio_->push(audio_out_.data(), audio_out_.size());
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
    bool stop_ = false;
    bool busy_ = false;
    bool in_flight_ = false;
    bool paused_ = false;
    bool pending_vol_ = false;
    bool pending_fm_ = false;
    bool fm_enabled_ = false;
    float music_vol_ = 1.0f;
    float sfx_vol_ = 1.0f;
    std::atomic<uint64_t> samples_total_{0};
    std::atomic<double> last_work_ms_{0.0};
};

} // namespace arm64
