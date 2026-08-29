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
// HAL-free: plain float arithmetic, host-testable.

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

class SvfFilter {
   public:
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

    float Process(float in) {
        if (bypass_)
            return in;
        const float v3 = in - ic2eq_;
        const float v1 = a1_ * ic1eq_ + a2_ * v3;
        const float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
        ic1eq_ = 2.0f * v1 - ic1eq_;
        ic2eq_ = 2.0f * v2 - ic2eq_;
        return v2;  // lowpass output
    }

    // Clears the integrator state without touching the tuning. Called at
    // Trigger() so a stolen voice cannot leak the previous note's filter
    // state into the new one as a click.
    void Reset() {
        ic1eq_ = 0.0f;
        ic2eq_ = 0.0f;
    }

   private:
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

    // Trapezoidal integrator state. NOTE for hardware bring-up: these decay
    // toward zero on silence and can reach denormal magnitudes, which are
    // slow on some FPUs. The Cortex-M7 FPU has a flush-to-zero mode (FPSCR.FZ)
    // that makes this free; confirm it is enabled rather than paying for a
    // per-sample guard here on a guess.
    float ic1eq_ = 0.0f;
    float ic2eq_ = 0.0f;
};

}  // namespace AudioEngine
}  // namespace WaveX
