#pragma once

// Control-rate LFO (roadmap Phase 2.5 item 4; design:
// docs/features/param-locks-and-modulation.md §5).
//
// Used two ways, from one class:
//   - Two ENGINE-GLOBAL LFOs, like a modular's LFO bank. Global on purpose:
//     slot 3's wobble must not change because slot 5 loaded a new instrument.
//   - One PER-VOICE LFO whose parameters come from the instrument, retriggered
//     at note-on with the classic E-mu delayed-vibrato ramp (delay then fade).
//
// Evaluated once per control tick and applied at block rate - never per
// sample. Everything downstream of a mod source in §3 already updates at block
// rate, so a per-sample LFO would cost eight times more for a resolution
// nothing consumes.
//
// HAL-free and allocation-free, so the waveforms, the phase wrap, the S&H
// sequence and the delay/fade envelope are all host-testable.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

enum class LfoWave : uint8_t {
    Sine = 0,
    Triangle = 1,
    Saw = 2,
    Square = 3,
    SampleHold = 4,
};

/// Rate limits from §5. The low end is slow enough for a filter sweep across
/// a bar; the high end stops short of audio rate, which this is not for.
static constexpr float kLfoMinRateHz = 0.02f;
static constexpr float kLfoMaxRateHz = 20.0f;

class Lfo {
   public:
    /// @param tick_hz control-tick rate (1 kHz on this engine).
    void Init(float tick_hz) {
        tick_hz_ = tick_hz > 0.0f ? tick_hz : 1000.0f;
        Reset();
    }

    void Reset() {
        phase_ = 0.0f;
        sh_value_ = 0.0f;
        elapsed_s_ = 0.0f;
        rng_ = 0x2545F491u;
        SampleAndHold();
        Recompute();
    }

    /// Recomputes immediately: a triangle or saw is at -1 when its phase is 0,
    /// so leaving Value() at whatever it was would make the first Tick() after
    /// a wave change look like a jump to anything reading the value in
    /// between. Every state change that can alter the output ends this way.
    void SetWave(LfoWave wave) {
        wave_ = wave;
        Recompute();
    }
    LfoWave Wave() const { return wave_; }

    void SetRateHz(float hz) {
        rate_hz_ = hz < kLfoMinRateHz ? kLfoMinRateHz : (hz > kLfoMaxRateHz ? kLfoMaxRateHz : hz);
    }
    float RateHz() const { return rate_hz_; }

    /**
     * @brief Delayed-vibrato envelope (§5): silence, then a fade in.
     *
     * One ramp and two parameters, which is the whole E-mu behaviour. Applied
     * as an amplitude on the output, so the LFO keeps running underneath and
     * a note held through the delay does not restart its phase.
     */
    void SetDelayFade(float delay_s, float fade_s) {
        delay_s_ = delay_s < 0.0f ? 0.0f : delay_s;
        fade_s_ = fade_s < 0.0f ? 0.0f : fade_s;
        Recompute();
    }

    /// Restarts phase and the delay/fade envelope. Called at note-on for the
    /// per-voice LFO, and by the transport for a phase-restart global LFO.
    void Retrigger() {
        phase_ = 0.0f;
        elapsed_s_ = 0.0f;
        SampleAndHold();
        Recompute();
    }

    /**
     * @brief Advances one control tick and returns the new value, -1..+1.
     *
     * The phase wraps by subtraction rather than fmod so a very slow LFO does
     * not accumulate the rounding a repeated modulo introduces, and S&H
     * resamples exactly on the wrap - which is what makes its steps land on
     * the period rather than drifting across it.
     */
    float Tick() {
        const float increment = rate_hz_ / tick_hz_;
        phase_ += increment;
        while (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            SampleAndHold();
        }
        elapsed_s_ += 1.0f / tick_hz_;
        Recompute();
        return value_;
    }

    /// Last value, without advancing. Block-rate consumers read this.
    float Value() const { return value_; }

    /// 0..1 phase, exposed for tests and for transport-synced restarts.
    float Phase() const { return phase_; }

   private:
    void Recompute() { value_ = Shape(phase_) * EnvelopeGain(); }

    /// Bipolar by convention: every shape returns -1..+1 so a mod depth means
    /// the same thing whichever waveform is selected.
    float Shape(float phase) const {
        switch (wave_) {
            case LfoWave::Sine:
                return std::sin(phase * 6.2831853071795865f);
            case LfoWave::Triangle:
                // 0 -> +1 -> 0 -> -1 -> 0
                return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
            case LfoWave::Saw:
                return 2.0f * phase - 1.0f;
            case LfoWave::Square:
                return phase < 0.5f ? 1.0f : -1.0f;
            case LfoWave::SampleHold:
                return sh_value_;
            default:
                return 0.0f;
        }
    }

    /// Delay then fade. Returns the amplitude the shape is scaled by.
    float EnvelopeGain() const {
        if (elapsed_s_ < delay_s_) {
            return 0.0f;
        }
        if (fade_s_ <= 0.0f) {
            return 1.0f;
        }
        const float into_fade = elapsed_s_ - delay_s_;
        if (into_fade >= fade_s_) {
            return 1.0f;
        }
        return into_fade / fade_s_;
    }

    /// Seeded xorshift so a test sees the same sequence every run. A
    /// non-deterministic S&H would make any assertion about it useless.
    void SampleAndHold() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        sh_value_ = (static_cast<float>(rng_ >> 8) / 8388608.0f) - 1.0f;
    }

    LfoWave wave_ = LfoWave::Sine;
    float rate_hz_ = 1.0f;
    float tick_hz_ = 1000.0f;
    float phase_ = 0.0f;
    float value_ = 0.0f;
    float sh_value_ = 0.0f;
    float delay_s_ = 0.0f;
    float fade_s_ = 0.0f;
    float elapsed_s_ = 0.0f;
    uint32_t rng_ = 0x2545F491u;
};

}  // namespace AudioEngine
}  // namespace WaveX
