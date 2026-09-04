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
// Hot *code* placement (WAVEX_ITCM_CODE) is deliberately NOT applied here.
// Render() is an inline member of a header that is compiled on the host, and
// the section attribute would have to be guarded per-target; more to the
// point, AGENTS.md requires a DWT number before claiming a placement win, and
// there isn't one yet. Tracked as a follow-up, not done on a hunch.
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
//   - Drive > 0 soft-clips the bandpass term v1 inside the integrator loop
//     before it is written back to the state. v1 is what the resonance
//     feedback path runs through, so this is where an analogue filter
//     saturates: the clipper only ever reduces |v1|, so the loop gain
//     cannot exceed the linear case and stability is preserved, while a
//     self-oscillating peak flattens into a rounded, level-limited tone
//     instead of a clean sine that grows without bound. The curve is the
//     cubic x - x^3/3 on [-1, 1] clamped to +-2/3 beyond it: unit slope at
//     zero (a quiet signal is untouched), continuous first derivative at the
//     clamp, and no divide, transcendental or table - four multiplies and
//     two compares per stage. Drive scales the signal into the clipper
//     (1x .. kMaxDriveGain x) and back out, so it sets how hot the
//     resonance has to be before it starts to fold.
//
// HAL-free: plain float arithmetic, host-testable.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class SvfFilter {
   public:
    enum class Slope : uint8_t { Db12, Db24 };

    // Pre-gain into the soft clipper at drive = 1.0. 8x means a bandpass
    // term of 1/8 full scale already starts to round; at drive just above 0
    // only a genuinely runaway resonance (|v1| toward 1) is touched.
    static constexpr float kMaxDriveGain = 8.0f;

    // Resonance 0..1 maps onto this Q range. 0.5 is well-damped (no peak at
    // all - a gentler knee than Butterworth, so resonance=0 sounds like a
    // plain lowpass rather than like a filter with a bump). kMaxQ is chosen
    // to be strongly resonant while staying comfortably stable: k = 1/Q is
    // still 0.05 at the top, and self-oscillation lives at k == 0.
    static constexpr float kMinQ = 0.5f;
    static constexpr float kMaxQ = 20.0f;

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
    void SetCutoff(float hz) {
        cutoff_hz_ = hz > 0.0f ? hz : 0.0f;
        UpdateCoeffs();
    }

    // 0 = no resonance, 1 = strongly resonant. Clamped rather than rejected:
    // this is fed from a wire parameter and a control tick, neither of which
    // should be able to destabilize a voice.
    void SetResonance(float res) {
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
        if (drive_ > 0.0f) {
            drive_gain_ = 1.0f + drive_ * (kMaxDriveGain - 1.0f);
            drive_gain_inv_ = 1.0f / drive_gain_;
        } else {
            drive_gain_ = 0.0f;  // sentinel: clipper off
            drive_gain_inv_ = 0.0f;
        }
    }
    float GetDrive() const { return drive_; }

    float Process(float in) {
        if (bypass_)
            return in;
        float out = Stage(in, ic1eq_, ic2eq_);
        if (slope_ == Slope::Db24) {
            out = Stage(out, ic3eq_, ic4eq_);
        }
        return out;  // lowpass output
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
    // One TPT lowpass stage over the given integrator pair. With the clipper
    // off this is exactly the original single-stage Process().
    float Stage(float in, float& ic1, float& ic2) const {
        const float v3 = in - ic2;
        float v1 = a1_ * ic1 + a2_ * v3;
        const float v2 = ic2 + a2_ * ic1 + a3_ * v3;
        if (drive_gain_ > 0.0f) {
            v1 = SoftClip(v1 * drive_gain_) * drive_gain_inv_;
        }
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }

    // x - x^3/3 on [-1, 1], +-2/3 outside: unit slope at 0, C1 at the clamp.
    static float SoftClip(float x) {
        if (x > 1.0f)
            return 2.0f / 3.0f;
        if (x < -1.0f)
            return -2.0f / 3.0f;
        return x - (x * x * x) * (1.0f / 3.0f);
    }

    void UpdateCoeffs() {
        const float nyquist = static_cast<float>(sample_rate_) * 0.5f;
        if (cutoff_hz_ >= nyquist) {
            bypass_ = true;
            return;
        }
        bypass_ = false;

        const float g =
            std::tan(3.14159265358979323846f * cutoff_hz_ / static_cast<float>(sample_rate_));
        const float q = kMinQ + resonance_ * (kMaxQ - kMinQ);
        const float k = 1.0f / q;

        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    uint32_t sample_rate_ = 48000;
    float cutoff_hz_ = 20000.0f;
    float resonance_ = 0.0f;
    bool bypass_ = true;

    // Coefficients (recomputed only when tuning changes, never per sample).
    float a1_ = 1.0f;
    float a2_ = 0.0f;
    float a3_ = 0.0f;

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
    float drive_gain_ = 0.0f;  // 0 = clipper off (see SetDrive)
    float drive_gain_inv_ = 0.0f;
};

}  // namespace AudioEngine
}  // namespace WaveX
