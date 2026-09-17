#pragma once
#include "audio/track_mix.hpp"
#include <array>
#include <cstdint>

namespace WaveX::AudioEngine {
// Foreground-only lease: a lost unsubscribe cannot leave telemetry running.
class MixMeterSubscription {
   public:
    void Subscribe(uint32_t now) {
        enabled_ = true;
        renewed_ = now;
    }
    void Unsubscribe() { enabled_ = false; }
    bool Enabled(uint32_t now) const {
        return enabled_ && static_cast<uint32_t>(now - renewed_) < 3000;
    }

   private:
    bool enabled_ = false;
    uint32_t renewed_ = 0;
};

// Callback-private peak hold across a telemetry window. VoiceManager adds
// post-strip voice contributions; no foreground reader touches this object.
class MixMeterWindow {
   public:
    void Init(uint32_t sample_rate) {
        window_ = sample_rate / 25;
        if (!window_)
            window_ = 1;
        Reset();
    }
    void Reset() {
        peaks_.fill(0);
        elapsed_ = 0;
    }
    float* Peaks() { return peaks_.data(); }
    const std::array<float, Mix::kNumTracks>& Values() const { return peaks_; }
    bool Advance(uint32_t frames) {
        elapsed_ += frames;
        return elapsed_ >= window_;
    }

   private:
    std::array<float, Mix::kNumTracks> peaks_{};
    uint32_t elapsed_ = 0, window_ = 1920;
};
struct MixMeterSnapshot {
    std::array<float, Mix::kNumTracks> peaks{};
    uint32_t sequence = 0;
};
}  // namespace WaveX::AudioEngine
