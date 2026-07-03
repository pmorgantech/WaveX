#pragma once

// Simple linear ADSR envelope generator (roadmap Phase 1 item 4, "ADSR per
// voice"). Deliberately not DaisySP's daisysp::Adsr - that class isn't
// portable to host tests (it isn't linked into the host test libraries),
// and a linear envelope is adequate for Phase 1; an exponential/curved
// shape is future polish, not a functional requirement here.
//
// HAL-free: plain float arithmetic, host-testable.

#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class Envelope {
   public:
    void Init(uint32_t sample_rate) { sample_rate_ = sample_rate > 0 ? sample_rate : 48000; }

    // Attack/decay/release are in seconds; sustain_level is 0..1. Times
    // <= 0 are treated as instantaneous (one-sample) transitions.
    void SetParams(float attack_s, float decay_s, float sustain_level, float release_s) {
        sustain_level_ =
            sustain_level < 0.0f ? 0.0f : (sustain_level > 1.0f ? 1.0f : sustain_level);
        attack_rate_ = RatePerSample(attack_s, 1.0f);
        decay_rate_ = RatePerSample(decay_s, 1.0f - sustain_level_);
        release_rate_ = RatePerSample(release_s, 1.0f);
    }

    // Starts (or restarts) the attack phase. Deliberately does not reset
    // `level_` to 0 - retriggering a voice that's mid-release (stolen and
    // reused) ramps up from wherever the envelope currently is, instead of
    // discontinuously jumping to 0 first.
    void Retrigger() { stage_ = Stage::Attack; }

    // Starts the release phase. No-op if already idle.
    void Release() {
        if (stage_ != Stage::Idle)
            stage_ = Stage::Release;
    }

    // Advances one sample, returns the new envelope level (0..1).
    float Process() {
        switch (stage_) {
            case Stage::Idle:
                return 0.0f;
            case Stage::Attack:
                level_ += attack_rate_;
                if (level_ >= 1.0f) {
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                }
                break;
            case Stage::Decay:
                level_ -= decay_rate_;
                if (level_ <= sustain_level_) {
                    level_ = sustain_level_;
                    stage_ = Stage::Sustain;
                }
                break;
            case Stage::Sustain:
                level_ = sustain_level_;
                break;
            case Stage::Release:
                level_ -= release_rate_;
                if (level_ <= 0.0f) {
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                }
                break;
        }
        return level_;
    }

    bool IsIdle() const { return stage_ == Stage::Idle; }
    bool IsReleasing() const { return stage_ == Stage::Release; }
    float Level() const { return level_; }

   private:
    enum class Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };

    float RatePerSample(float time_s, float span) const {
        if (time_s <= 0.0f)
            return span;  // instantaneous: reach the target in one Process() call
        return span / (time_s * static_cast<float>(sample_rate_));
    }

    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    uint32_t sample_rate_ = 48000;
    float attack_rate_ = 1.0f;
    float decay_rate_ = 1.0f;
    float sustain_level_ = 1.0f;
    float release_rate_ = 1.0f;
};

}  // namespace AudioEngine
}  // namespace WaveX
