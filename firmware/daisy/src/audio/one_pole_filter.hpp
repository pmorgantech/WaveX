#pragma once

// One-pole lowpass filter: the digital stand-in for the analog VCF called
// for in architecture.md §5.2 ("one-pole/SVF digital filter as stand-in
// until the analog board exists") and roadmap Phase 1 item 4. Picked
// one-pole over SVF for this pass - it's the simplest option that satisfies
// "a stand-in," is trivially portable/host-testable, and needs no per-voice
// resonance state; upgrading to an SVF (for resonance) is a drop-in
// follow-up once there's a reason to need it.
//
// HAL-free: plain float arithmetic, host-testable.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class OnePoleFilter {
   public:
    void Init(uint32_t sample_rate) { sample_rate_ = sample_rate > 0 ? sample_rate : 48000; }

    // Standard one-pole coefficient: coeff = 1 - exp(-2*pi*fc/fs). A cutoff
    // at or above Nyquist is physically meaningless for a lowpass (there's
    // nothing left to attenuate), so it's treated as an exact bypass
    // (coeff = 1.0) rather than asymptotically approaching but never
    // reaching full-open - avoids audible/measurable attenuation from
    // callers that just want "no filtering."
    void SetCutoff(float hz) {
        if (hz < 0.0f)
            hz = 0.0f;
        const float nyquist = static_cast<float>(sample_rate_) * 0.5f;
        if (hz >= nyquist) {
            coeff_ = 1.0f;
            return;
        }
        float x = -2.0f * 3.14159265358979323846f * hz / static_cast<float>(sample_rate_);
        coeff_ = 1.0f - std::exp(x);
        if (coeff_ < 0.0f)
            coeff_ = 0.0f;
        if (coeff_ > 1.0f)
            coeff_ = 1.0f;
    }

    float Process(float in) {
        state_ += coeff_ * (in - state_);
        return state_;
    }

    void Reset() { state_ = 0.0f; }

   private:
    uint32_t sample_rate_ = 48000;
    float coeff_ = 1.0f;  // 1.0 = fully open (no filtering)
    float state_ = 0.0f;
};

}  // namespace AudioEngine
}  // namespace WaveX
