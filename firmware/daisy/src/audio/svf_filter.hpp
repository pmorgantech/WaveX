#pragma once

// Topology-preserving-transform state-variable lowpass, per voice. Replaces
// the one-pole stand-in (one_pole_filter.hpp) that this file supersedes -
// that header's own comment anticipated the swap ("upgrading to an SVF (for
// resonance) is a drop-in follow-up once there's a reason to need it").
//
// The reason: PARAM_FILTER_RESONANCE had no digital consumer at all, because
// a one-pole has no resonance state to give it. A filter that cannot resonate
// barely exercises the voice architecture, and the all-digital audition path
// (features/digital-voice-audition.md stage 1) exists to exercise exactly
// that.
//
// WHY TPT/SVF RATHER THAN A CMSIS-DSP BIQUAD. AGENTS.md prefers CMSIS-DSP
// kernels wherever they cover the operation, and arm_biquad_cascade_df1_f32
// does cover "resonant lowpass" - but it covers the *static* case. It is a
// block kernel with fixed coefficients, so using it would mean (a)
// restructuring Render() to filter a whole block per voice through a scratch
// buffer rather than one sample at a time inside the existing per-sample
// loop, and (b) recomputing biquad coefficients whenever cutoff moves. Direct
// -form biquads are also poorly behaved when their coefficients are modulated
// - the state no longer means what it meant under the previous coefficients,
// which is audible as zipper noise and, at high Q, as blow-ups.
//
// The TPT structure below is specifically designed so cutoff and resonance
// can change between samples without the state going stale, which is the
// entire point of stage 1: sweeping the filter *while a note sounds*. That is
// a correctness property here, not a preference.
//
// RETUNE COST (2026-09-14). g = tan(pi fc/fs) comes from fast_tan.hpp: the
// stmlib polynomial below 12 kHz (within 0.25% of tan), the real tan above.
// A retune under cutoff modulation on eight voices no longer costs eight
// library tan() calls per block.
//
// The block-kernel route stays open as a measured optimization: if the DWT
// numbers say the per-sample SVF is too expensive at 8 voices, the answer is
// to restructure Render() around block processing, not to give up modulation.
// Do not swap it on a hunch - AGENTS.md requires a number.
//
// Reference: Andrew Simper / Cytomic, "Solving the continuous SVF equations
// using trapezoidal integration"; equivalently Zavalishin, "The Art of VA
// Filter Design" ch. 3-4.
//
// MEMORY PLACEMENT. This filter's state and coefficients are already in DTCM
// and need no annotation of their own: an SvfFilter lives inside Voice, which
// lives inside VoiceManager::voices_, and audio_engine.cpp declares
// `s_voice_manager WAVEX_DTCM_DATA`. Placement is inherited by containment,
// which is exactly what you want for state written every sample by every
// active voice from Callback() and never touched by DMA (DMA1/DMA2 cannot
// reach DTCM at all).
//
// Two ways to lose that accidentally, both worth guarding against:
//   - Giving this class STATIC storage of its own - a shared coefficient
//     lookup table, say - would put that table in .bss (cacheable AXI SRAM),
//     not DTCM, however hot it is. That is one reason the coefficients below
//     are computed on tuning change rather than tabulated: a per-instance
//     float costs 4 bytes of DTCM, a shared table costs a cache miss in the
//     inner loop.
//   - Heap-allocating or otherwise hoisting a filter out of VoiceManager
//     would silently drop it out of DTCM with no diagnostic.
//
// VoiceManager's renderer and trigger path carry the selective ITCM
// annotation. Inlined filter operations inherit that code placement; the
// filter has no separate section or storage owner. DWT comparisons and the
// whole-callback gate are recorded in docs/callback-performance-log.md.
//
// SLOPE AND DRIVE (2026-09-04). Two things a linear 12 dB/oct TPT lowpass
// cannot do that a "synth filter" is expected to: fall off at 24 dB/oct, and
// misbehave gracefully at high resonance. Both are options here, both default
// OFF so that a filter nobody configured is bit-identical to the linear
// 12 dB version the tests and the voice path were written against.
//
//   - Slope::Db24 runs a second, identically tuned TPT stage in series.
//     Twice the cost (~10 more flops per sample), the classic 4-pole
//     lowpass shape. Both stages share one coefficient set, so modulation
//     still costs one tan() per change.
//
//   - Drive (rewritten 2026-09-14) does what drive does on the ladders and
//     on the bench: more of it is louder and dirtier, never duller. It is
//     an input stage only: the signal is raised by the same passband-
//     weighted gain law the ladders use (1x at drive 0 to 2.5x at drive 1)
//     and soft-saturated, blended in by the drive amount so drive 0 is the
//     linear filter bit for bit and 1% is a whisper away from it. The
//     curve is one cubic, x - x^3/6.75 on [-1.5, 1.5] clamped to +-1
//     beyond it: unit slope at zero, unit ceiling, continuous first
//     derivative at the clamp, no divide, transcendental or table.
//     Nothing touches the integrator loop. Two earlier versions did, and
//     both were wrong in the same way: a per-sample limiter on the
//     bandpass state damps a high-Q ring drastically even when blended in
//     at 1% (resonance 0.7 is Q 14 here, and the state runs far above
//     unity), which heard as a level jump at the first click of drive and
//     as "more drive, quieter". The resonance stays exactly as linear as
//     the resonance setting asks for; stability comes from the Q clamp.
//
// HAL-free: plain float arithmetic, host-testable.

#include "fast_tan.hpp"
#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class SvfFilter {
   public:
    enum class Slope : uint8_t { Db12, Db24 };
    enum class Mode : uint8_t { LowPass = 0, HighPass = 1, BandPass = 2, Notch = 3 };
    void SetMode(Mode mode) {
        if (static_cast<uint8_t>(mode) > 3)
            mode = Mode::LowPass;
        if (mode != mode_) {
            // Stage 1 has the same input in every mode; stage 2 does not.
            ic3eq_ = ic4eq_ = 0;
        }
        mode_ = mode;
    }
    Mode GetMode() const { return mode_; }

    // Pre-gain into the soft clipper at drive = 1.0. 8x means a bandpass
    // term of 1/8 full scale already starts to round; at drive just above 0
    // only a genuinely runaway resonance (|v1| toward 1) is touched.
    // Input gain at full drive: DaisySP's ladder law, 1 + (4 - 1) * (1 - 0.5).
    static constexpr float kMaxDriveGain = 2.5f;

    // Resonance 0..1 maps onto this Q range along a cubic, Q = 0.5 + 15.5 r^3,
    // so it tracks the ladder's feel under the one RES control (2026-09-14,
    // tuned on the bench): 0.5 is well-damped (no peak at all - a gentler
    // knee than Butterworth, so resonance=0 sounds like a plain lowpass
    // rather than like a filter with a bump), 50% is +7 dB, 70% +15 dB,
    // 100% is Q 16 (+24 dB). The ladder sits near +13 dB at 70% and
    // self-oscillates from 74%; the SVF cannot self-oscillate (k = 1/Q is
    // still 0.06 at the top), so its top is simply very resonant instead.
    static constexpr float kMinQ = 0.5f;
    static constexpr float kMaxQ = 16.0f;
    // The 24 dB cascade's second stage is a fixed Butterworth pair (Q 0.707):
    // resonance lives in the first stage only, so a held tone at the cutoff
    // peaks at Q, not Q^2 (400x at the old top), and the second stage adds
    // slope without a second bump.
    static constexpr float kSecondStageQ = 0.70710678f;

    void Init(uint32_t sample_rate) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        UpdateCoeffs();
    }

    // A cutoff at or above Nyquist is physically meaningless for a lowpass
    // (there is nothing left to attenuate), so it is an EXACT bypass -
    // Process() returns its input untouched, resonance included. This
    // contract is inherited deliberately from the one-pole: VoiceTriggerParams
    // documents "effectively open/bypass by default" and the voice-manager
    // tests set filter_cutoff_hz = 1e6 to mean "no filtering at all". An
    // asymptotic approach to open would leave measurable attenuation on every
    // voice that just wants a dry sample.
    void SetCutoff(float hz) { SetParameters(hz, resonance_); }

    // 0 = no resonance, 1 = strongly resonant. Clamped rather than rejected:
    // this is fed from a wire parameter and a control tick, neither of which
    // should be able to destabilize a voice.
    void SetResonance(float res) { SetParameters(cutoff_hz_, res); }

    // Cutoff and resonance define one coefficient set. Callers holding
    // both values need one tan()/coefficient calculation, not two.
    // Integrator state is untouched, exactly as with the separate setters.
    void SetParameters(float hz, float res) {
        cutoff_hz_ = hz > 0.0f ? hz : 0.0f;
        resonance_ = res < 0.0f ? 0.0f : (res > 1.0f ? 1.0f : res);
        UpdateCoeffs();
    }

    // 12 or 24 dB/oct. Takes effect on the next sample; the second stage's
    // state is cleared when switching so a stage that was not running does
    // not start from stale content.
    void SetSlope(Slope slope) {
        if (slope != slope_) {
            ic3eq_ = 0.0f;
            ic4eq_ = 0.0f;
        }
        slope_ = slope;
    }
    Slope GetSlope() const { return slope_; }

    // 0 = linear, exactly the filter as it was; up to 1 = the bandpass term
    // is driven kMaxDriveGain x into the soft clipper. Clamped.
    void SetDrive(float drive) {
        drive_ = drive < 0.0f ? 0.0f : (drive > 1.0f ? 1.0f : drive);
        drive_gain_ = 1.0f + drive_ * (kMaxDriveGain - 1.0f);
    }
    float GetDrive() const { return drive_; }

    float Process(float in) {
        if (boundary_) {
            const bool pass = boundary_ == 2 ? mode_ == Mode::LowPass || mode_ == Mode::Notch
                                             : mode_ == Mode::HighPass || mode_ == Mode::Notch;
            return pass ? in : 0.f;
        }
        float x = in;
        if (drive_ > 0.0f) {
            // Blended so the linear filter is recovered exactly at 0 and
            // approached continuously; the cubic's slope is 1 at the origin.
            x = in + drive_ * (Saturate(in * drive_gain_) - in);
        }
        float out = Stage(x, ic1eq_, ic2eq_, a1_, a2_, a3_, damping_);
        if (slope_ == Slope::Db24) {
            out = Stage(out, ic3eq_, ic4eq_, b1_, b2_, b3_, kSecondStageK);
        }
        return out;
    }

    // Clears the integrator state without touching the tuning. Called at
    // Trigger() so a stolen voice cannot leak the previous note's filter
    // state into the new one as a click.
    void Reset() {
        ic1eq_ = 0.0f;
        ic2eq_ = 0.0f;
        ic3eq_ = 0.0f;
        ic4eq_ = 0.0f;
    }

   private:
    // One TPT stage over the given integrator pair and coefficient set:
    // exactly the original single-stage Process() for the first stage.
    float Stage(float in, float& ic1, float& ic2, float a1, float a2, float a3, float k) const {
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        if (mode_ == Mode::LowPass)
            return v2;
        if (mode_ == Mode::BandPass)
            return v1;
        const float notch = in - k * v1;
        return mode_ == Mode::Notch ? notch : notch - v2;
    }

    // x - x^3/6.75 on [-1.5, 1.5], +-1 outside: unit slope at 0, unit
    // ceiling, C1 at the clamp. (The cubic x - x^3/3 scaled by 1.5.)
    static float Saturate(float x) {
        if (x > 1.5f)
            return 1.0f;
        if (x < -1.5f)
            return -1.0f;
        return x - (x * x * x) * (1.0f / 6.75f);
    }

    void UpdateCoeffs() {
        const float nyquist = static_cast<float>(sample_rate_) * 0.5f;
        if (cutoff_hz_ >= nyquist) {
            boundary_ = 2;
            return;
        }
        if (cutoff_hz_ <= 0) {
            boundary_ = 1;
            return;
        }
        boundary_ = 0;

        const float g = TanPi(cutoff_hz_ / static_cast<float>(sample_rate_));
        const float r3 = resonance_ * resonance_ * resonance_;
        const float q = kMinQ + r3 * (kMaxQ - kMinQ);
        const float k = 1.0f / q;
        damping_ = k;

        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
        // Second stage: same g, fixed Butterworth damping.
        b1_ = 1.0f / (1.0f + g * (g + kSecondStageK));
        b2_ = g * b1_;
        b3_ = g * b2_;
    }
    static constexpr float kSecondStageK = 1.0f / kSecondStageQ;

    uint32_t sample_rate_ = 48000;
    float cutoff_hz_ = 20000.0f;
    float resonance_ = 0.0f;
    uint8_t boundary_ = 2;  // 0 normal, 1 zero cutoff, 2 Nyquist/open
    Mode mode_ = Mode::LowPass;
    float damping_ = 2;

    // Coefficients (recomputed only when tuning changes, never per sample).
    float a1_ = 1.0f;
    float a2_ = 0.0f;
    float a3_ = 0.0f;
    float b1_ = 1.0f, b2_ = 0.0f, b3_ = 0.0f;  // second (24 dB) stage

    // Trapezoidal integrator state. These decay toward zero on silence and
    // can reach subnormal magnitudes; the Cortex-M7's FPv5 handles subnormals
    // in hardware at full speed, so no flush-to-zero mode or per-sample guard
    // is needed on this target (the concern is real on x86 hosts only).
    float ic1eq_ = 0.0f;
    float ic2eq_ = 0.0f;
    // Second stage, used only at Slope::Db24.
    float ic3eq_ = 0.0f;
    float ic4eq_ = 0.0f;

    Slope slope_ = Slope::Db12;
    float drive_ = 0.0f;
    float drive_gain_ = 1.0f;  // input gain, 1..kMaxDriveGain
};

}  // namespace AudioEngine
}  // namespace WaveX
