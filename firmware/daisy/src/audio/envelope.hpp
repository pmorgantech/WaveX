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

    // Reconfigures just the release time (seconds), leaving A/D/S untouched.
    // Used for choke groups (instrument-model.md §3): a choked voice gets a
    // very short release forced onto it before Release(), so open/closed-hat
    // cutoffs are near-instant but still click-free.
    void SetReleaseTime(float release_s) { release_rate_ = RatePerSample(release_s, 1.0f); }

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

    // Advances by `n` samples at once and returns the resulting level - the
    // same result Process() called n times in a row would give, but in a
    // handful of stage-transition steps rather than a per-sample loop.
    //
    // For a MODULATION SOURCE (a second envelope feeding a control-rate
    // destination, param-locks-and-modulation.md §4/§3) that only needs to
    // change once per block: nothing reads its value between samples, so a
    // per-sample loop would cost exactly as much as the audio-rate amp
    // envelope for no observable benefit. Process() itself is unchanged and
    // still the right choice for anything that's actually heard.
    float AdvanceBlock(uint32_t n) {
        float remaining = static_cast<float>(n);
        while (remaining > 0.0f) {
            switch (stage_) {
                case Stage::Idle:
                    return 0.0f;
                case Stage::Sustain:
                    level_ = sustain_level_;
                    return level_;
                case Stage::Attack: {
                    const float rate = attack_rate_ > 0.0f ? attack_rate_ : 1.0f;
                    const float to_go = (1.0f - level_) / rate;
                    if (remaining < to_go) {
                        level_ += rate * remaining;
                        return level_;
                    }
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                    remaining -= to_go;
                    break;
                }
                case Stage::Decay: {
                    if (decay_rate_ <= 0.0f) {
                        // Zero span (sustain_level_ == 1.0): Process() would
                        // fall through to Sustain on its very next call
                        // without spending a sample.
                        level_ = sustain_level_;
                        stage_ = Stage::Sustain;
                        break;
                    }
                    const float to_go = (level_ - sustain_level_) / decay_rate_;
                    if (remaining < to_go) {
                        level_ -= decay_rate_ * remaining;
                        return level_;
                    }
                    level_ = sustain_level_;
                    stage_ = Stage::Sustain;
                    remaining -= to_go;
                    break;
                }
                case Stage::Release: {
                    const float rate = release_rate_ > 0.0f ? release_rate_ : 1.0f;
                    const float to_go = level_ / rate;
                    if (remaining < to_go) {
                        level_ -= rate * remaining;
                        return level_;
                    }
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                    return 0.0f;
                }
            }
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
