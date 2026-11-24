// Adaptive-rate SDL2 audio output for ProjectKagemaru (48 kHz m1audio -> 48 kHz SDL2).
// Smoothly absorbs 57.524 Hz <-> 60 Hz pacing differences without crackles.
#pragma once

#ifdef VF1_VULKAN
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace arm64 {

class AudioRate {
public:
    static constexpr int kTargetFrames = 3072; // ~64 ms @ 48 kHz
    static constexpr int kPrimeFrames = 2560;  // ~53 ms before unpausing
    static constexpr double kMaxSlowDown = 0.05;
    static constexpr double kMaxSpeedUp = 0.01;
    static constexpr double kGain = 0.05;
    static constexpr double kIntegral = 0.0002;
    static constexpr double kSmoothing = 0.05;
    static constexpr double kSlew = 0.0005;

    void reset(int level) { average_ = double(level); }
    double update(int level) {
        average_ += (double(level) - average_) * kSmoothing;
        const double error = (average_ - double(kTargetFrames)) / double(kTargetFrames);
        integral_ = std::clamp(integral_ + error * kIntegral, -kMaxSlowDown, kMaxSpeedUp);
        const double want = 1.0 + std::clamp(error * kGain + integral_, -kMaxSlowDown, kMaxSpeedUp);
        ratio_ += std::clamp(want - ratio_, -kSlew, kSlew);
        return ratio_;
    }
    double ratio() const { return ratio_; }
    double average() const { return average_; }

private:
    double average_ = double(kTargetFrames);
    double ratio_ = 1.0;
    double integral_ = 0.0;
};

class StereoResampler {
public:
    void reset() {
        prev_[0] = prev_[1] = cur_[0] = cur_[1] = 0.0f;
        frac_ = 0.0;
    }
    int needed(int out_frames, double ratio) const {
        double frac = frac_;
        int consumed = 0;
        for (int i = 0; i < out_frames; ++i) {
            frac += ratio;
            while (frac >= 1.0) { frac -= 1.0; ++consumed; }
        }
        return consumed;
    }
    int run(const int16_t *in, int in_frames, float *out, int out_frames, double ratio) {
        int used = 0;
        for (int i = 0; i < out_frames; ++i) {
            const float t = float(frac_);
            out[2 * i] = prev_[0] + (cur_[0] - prev_[0]) * t;
            out[2 * i + 1] = prev_[1] + (cur_[1] - prev_[1]) * t;
            frac_ += ratio;
            while (frac_ >= 1.0) {
                frac_ -= 1.0;
                prev_[0] = cur_[0];
                prev_[1] = cur_[1];
                if (used < in_frames) {
                    cur_[0] = float(in[2 * used]) * (1.0f / 32768.0f);
                    cur_[1] = float(in[2 * used + 1]) * (1.0f / 32768.0f);
                    ++used;
                } else {
                    cur_[0] *= 0.992f;
                    cur_[1] *= 0.992f;
                }
            }
        }
        return used;
    }

private:
    float prev_[2] = {}, cur_[2] = {};
    double frac_ = 0.0;
};

class Audio {
public:
    struct Stats {
        unsigned underruns = 0;
        double ratio = 1.0;
        double queued_ms = 0.0;
    };

    Audio() = default;
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    ~Audio() { close(); }

    bool open(int source_rate = 48000) {
        source_rate_ = source_rate;
#ifdef VF1_VULKAN
        SDL_AudioSpec want{SDL_AUDIO_S16, 2, 48000};
        dev_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, sdl3_callback, this);
        if (!dev_stream_) return false;
        rate_ = 48000;
        SDL_AudioSpec src_spec{SDL_AUDIO_S16, 2, source_rate_};
        stream_ = SDL_CreateAudioStream(&src_spec, &src_spec);
        if (!stream_) { close(); return false; }
        SDL_LockAudioStream(dev_stream_);
        restart_locked();
        SDL_UnlockAudioStream(dev_stream_);
        return true;
#else
        SDL_AudioSpec want{}, got{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = callback;
        want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (!device_) return false;
        rate_ = got.freq > 0 ? got.freq : 48000;
        stream_ = SDL_NewAudioStream(AUDIO_S16SYS, 2, source_rate_, AUDIO_S16SYS, 2, rate_);
        if (!stream_) { close(); return false; }
        restart_locked();
        return true;
#endif
    }

    void push(const int16_t *samples, size_t count) {
#ifdef VF1_VULKAN
        if (!dev_stream_ || !stream_ || !samples || !count) return;
        SDL_LockAudioStream(dev_stream_);
        const int max_bytes = rate_ * kFrameBytes / 4;
        if (SDL_GetAudioStreamAvailable(stream_) > max_bytes) {
            SDL_ClearAudioStream(stream_);
        }
        SDL_PutAudioStreamData(stream_, samples, int(count * sizeof(int16_t)));
        const int prime = AudioRate::kPrimeFrames * kFrameBytes;
        const bool ready = SDL_GetAudioStreamAvailable(stream_) >= prime;
        SDL_UnlockAudioStream(dev_stream_);
        if (!playing_ && ready) {
            SDL_ResumeAudioStreamDevice(dev_stream_);
            playing_ = true;
        }
#else
        if (!device_ || !stream_ || !samples || !count) return;
        SDL_LockAudioDevice(device_);
        const int max_bytes = rate_ * kFrameBytes / 4; // cap at 250 ms
        if (SDL_AudioStreamAvailable(stream_) > max_bytes) {
            SDL_AudioStreamClear(stream_);
        }
        SDL_AudioStreamPut(stream_, samples, int(count * sizeof(int16_t)));
        const int prime = AudioRate::kPrimeFrames * kFrameBytes;
        const bool ready = SDL_AudioStreamAvailable(stream_) >= prime;
        SDL_UnlockAudioDevice(device_);
        if (!playing_ && ready) {
            SDL_PauseAudioDevice(device_, 0);
            playing_ = true;
        }
#endif
    }

    void pause() {
#ifdef VF1_VULKAN
        if (!dev_stream_) return;
        SDL_PauseAudioStreamDevice(dev_stream_);
        SDL_LockAudioStream(dev_stream_);
        if (stream_) SDL_ClearAudioStream(stream_);
        restart_locked();
        SDL_UnlockAudioStream(dev_stream_);
        playing_ = false;
#else
        if (!device_) return;
        SDL_PauseAudioDevice(device_, 1);
        SDL_LockAudioDevice(device_);
        if (stream_) SDL_AudioStreamClear(stream_);
        restart_locked();
        SDL_UnlockAudioDevice(device_);
        playing_ = false;
#endif
    }

    int queued_frames() {
#ifdef VF1_VULKAN
        if (!dev_stream_) return 0;
        SDL_LockAudioStream(dev_stream_);
        const int q = queued_frames_locked();
        SDL_UnlockAudioStream(dev_stream_);
        return q;
#else
        if (!device_) return 0;
        SDL_LockAudioDevice(device_);
        const int q = queued_frames_locked();
        SDL_UnlockAudioDevice(device_);
        return q;
#endif
    }

    Stats stats() {
        Stats out;
#ifdef VF1_VULKAN
        if (!dev_stream_) return out;
        SDL_LockAudioStream(dev_stream_);
        out.underruns = underruns_;
        out.ratio = rate_control_.ratio();
        out.queued_ms = rate_control_.average() * 1000.0 / double(rate_);
        SDL_UnlockAudioStream(dev_stream_);
        return out;
#else
        if (!device_) return out;
        SDL_LockAudioDevice(device_);
        out.underruns = underruns_;
        out.ratio = rate_control_.ratio();
        out.queued_ms = rate_control_.average() * 1000.0 / double(rate_);
        SDL_UnlockAudioDevice(device_);
        return out;
#endif
    }

    void volume(float value) {
#ifdef VF1_VULKAN
        if (dev_stream_) SDL_LockAudioStream(dev_stream_);
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (dev_stream_) SDL_UnlockAudioStream(dev_stream_);
#else
        if (device_) SDL_LockAudioDevice(device_);
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (device_) SDL_UnlockAudioDevice(device_);
#endif
    }

    void mute(bool muted) {
#ifdef VF1_VULKAN
        if (dev_stream_) SDL_LockAudioStream(dev_stream_);
        muted_ = muted;
        if (dev_stream_) SDL_UnlockAudioStream(dev_stream_);
#else
        if (device_) SDL_LockAudioDevice(device_);
        muted_ = muted;
        if (device_) SDL_UnlockAudioDevice(device_);
#endif
    }

    void close() {
#ifdef VF1_VULKAN
        if (dev_stream_) SDL_DestroyAudioStream(dev_stream_);
        dev_stream_ = nullptr;
        if (stream_) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        playing_ = false;
#else
        if (device_) SDL_CloseAudioDevice(device_);
        device_ = 0;
        if (stream_) SDL_FreeAudioStream(stream_);
        stream_ = nullptr;
        playing_ = false;
#endif
    }

private:
    static constexpr int kFrameBytes = 2 * int(sizeof(int16_t));
    static constexpr int kChunk = 512;
    static constexpr int kMaxInput = kChunk * 2;

    void restart_locked() {
        buffering_ = true;
        resampler_.reset();
    }
    int queued_frames_locked() const {
#ifdef VF1_VULKAN
        return stream_ ? SDL_GetAudioStreamAvailable(stream_) / kFrameBytes : 0;
#else
        return stream_ ? SDL_AudioStreamAvailable(stream_) / kFrameBytes : 0;
#endif
    }

#ifdef VF1_VULKAN
    static void SDLCALL sdl3_callback(void *userdata, SDL_AudioStream *astream, int additional_amount, int /*total_amount*/) {
        if (additional_amount <= 0) return;
        Uint8 buf[kChunk * kFrameBytes];
        int rem = additional_amount;
        while (rem > 0) {
            const int n = std::min(rem, int(sizeof(buf)));
            callback(userdata, buf, n);
            SDL_PutAudioStreamData(astream, buf, n);
            rem -= n;
        }
    }
#endif

    static void callback(void *userdata, Uint8 *buffer, int bytes) {
        auto &self = *static_cast<Audio *>(userdata);
        std::memset(buffer, 0, size_t(bytes));
        auto *out = reinterpret_cast<int16_t *>(buffer);
        int frames = bytes / kFrameBytes;
        int queued = self.queued_frames_locked();
        if (self.buffering_) {
            if (queued < AudioRate::kPrimeFrames) return;
            self.buffering_ = false;
            self.rate_control_.reset(queued);
        }
        const double ratio = self.rate_control_.update(queued);
        while (frames > 0) {
            const int n = std::min(frames, kChunk);
            const int need = self.resampler_.needed(n, ratio);
            int got = std::min(need, kMaxInput);
            if (need > queued) {
                ++self.underruns_;
                got = std::min(queued, kMaxInput);
            }
            if (got > 0) {
#ifdef VF1_VULKAN
                SDL_GetAudioStreamData(self.stream_, self.in_buf_, got * kFrameBytes);
#else
                SDL_AudioStreamGet(self.stream_, self.in_buf_, got * kFrameBytes);
#endif
            }
            queued -= got;
            self.resampler_.run(self.in_buf_, got, self.mixed_, n, ratio);
            for (int i = 0; i < n * 2; ++i) {
                float sample = self.muted_ ? 0.0f : self.mixed_[i] * self.volume_;
                if (!std::isfinite(sample)) sample = 0.0f;
                out[i] = int16_t(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
            }
            frames -= n;
            out += n * 2;
        }
    }

#ifdef VF1_VULKAN
    SDL_AudioStream *dev_stream_ = nullptr;
#else
    SDL_AudioDeviceID device_ = 0;
#endif
    SDL_AudioStream *stream_ = nullptr;
    int source_rate_ = 48000;
    int rate_ = 48000;
    bool playing_ = false;
    bool muted_ = false;
    float volume_ = 0.9f;
    AudioRate rate_control_;
    StereoResampler resampler_;
    bool buffering_ = true;
    unsigned underruns_ = 0;
    int16_t in_buf_[kMaxInput * 2] = {};
    float mixed_[kChunk * 2] = {};
};

} // namespace arm64
