#pragma once
#include <cmath>
#include <cstdint>

namespace WaveX::AudioEngine {
// Callback-owned, fixed 5 ms linear ramp. Next() is fused into the existing
// output/meter traversal; no additional buffer or full audio traversal.
class MasterGain {
   public:
    void Init(float sample_rate) {
        frames_ = static_cast<uint32_t>(sample_rate * 0.005f);
        if (!frames_)
            frames_ = 1;
        current_ = target_ = 1.0f;
        remaining_ = 0;
        step_ = 0;
    }
    void SetTarget(float gain) {
        if (!std::isfinite(gain) || gain < 0 || gain == target_)
            return;
        target_ = gain;
        remaining_ = frames_;
        step_ = (target_ - current_) / static_cast<float>(frames_);
    }
    float Next() {
        if (remaining_) {
            current_ += step_;
            if (!--remaining_)
                current_ = target_;
        }
        return current_;
    }

   private:
    float current_ = 1, target_ = 1, step_ = 0;
    uint32_t frames_ = 240, remaining_ = 0;
};
}  // namespace WaveX::AudioEngine
