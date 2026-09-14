#pragma once

// The per-voice filter, selectable per Instrument between two topologies
// (Protocol::InstFilterTopology, carried by InstrumentFilter::topology):
//
//   FilterTopology::WaveXSvf - svf_filter.hpp: the first-party TPT
//                              state-variable filter. LP/HP/BP/Notch, 12 or
//                              24 dB, input-stage drive.
//   FilterTopology::Ladder   - ladder_zdf.hpp: a first-party zero-delay-
//                              feedback Moog ladder, exact tuning with no
//                              oversampling and one saturation per sample.
//                              About the cost of the 24 dB SVF.
//
// HISTORY. Two more ladders were built and measured on 2026-09-14 - a port
// of the DaisySP/Teensy Huovilainen ladder at 4x and 2x oversampling - and
// pruned the same day: at listening settings all three were the same filter
// to within -35 dB, the 4x model cost ~40 points of the block budget for
// eight voices against the ZDF's ~3, and the only audible divergence was at
// high cutoff with full resonance and drive on the high-pass taps. The
// numbers are in docs/callback-performance-log.md; wire values 2 and 3 are
// retired, not reused.
//
// The ladder renders LP/BP/HP at 12 or 24 dB from its stage taps; Notch is
// not a ladder response and is rendered as input minus the 12 dB band-pass
// tap, a true null at the cutoff and only an approximation of a notch skirt
// away from it.
//
// Topology, mode, slope and drive are Instrument-owned voice character:
// they arrive on VoiceTriggerParams for the next note and through
// VoiceInstrumentParams for held ones. FilterConfig (slope, drive) applies
// to whichever topology is active.
//
// Contracts kept identical across both:
//   - cutoff at or above Nyquist is an EXACT bypass for LP/Notch and silence
//     for HP/BP, and cutoff at or below zero the reverse (the voice-manager
//     tests rely on it; the ladder would otherwise clamp to its top and
//     still filter), so it is handled here before either sees it;
//   - resonance 0..1, cutoff in Hz, both callable at block rate from the
//     callback: no allocation, only the tuning arithmetic each already pays
//     (fast_tan.hpp's polynomial) and only when the tuning changes.
//
// RESONANCE, ONE CONTROL FOR BOTH (2026-09-14, tuned on the bench). The
// ladder's loop gain is 5.4 * resonance: it self-oscillates from about 74%
// and holds a clean, saturation-bounded tone above that. The SVF cannot
// self-oscillate by construction (its Q clamp is the ceiling), so its cubic
// Q curve is set to meet the ladder's peak height through the middle of the
// travel: both are near +13..15 dB at 70%.
//
// Only the ACTIVE topology is retuned on SetCutoff/SetResonance; the other
// is brought up to date when switched in, so an idle topology costs nothing
// per block. A switch clears the incoming filter's state so the previous
// topology cannot leak through as a click.
//
// Both live inside Voice, so they inherit VoiceManager's DTCM placement
// (see svf_filter.hpp on why that matters).
//
// HAL-free: plain float arithmetic, host-testable.

#include "ladder_zdf.hpp"
#include "svf_filter.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

// Values match Protocol::InstFilterTopology (static_asserted in instrument.hpp).
enum class FilterTopology : uint8_t { WaveXSvf = 0, Ladder = 1 };
static constexpr uint8_t kFilterTopologyCount = 2;

// Slope and drive. Both default OFF so a filter nobody configured is the
// linear 12 dB one the tests were written against.
struct FilterConfig {
    SvfFilter::Slope slope = SvfFilter::Slope::Db12;
    float drive = 0.0f;  // 0..1; SVF input stage, ladder input drive

    bool operator==(const FilterConfig& o) const { return slope == o.slope && drive == o.drive; }
    bool operator!=(const FilterConfig& o) const { return !(*this == o); }
};

class VoiceFilter {
   public:
    void Init(uint32_t sample_rate) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        mine_.Init(sample_rate_);
        ladder_.Init(static_cast<float>(sample_rate_));
        config_ = FilterConfig{};
        topology_ = FilterTopology::WaveXSvf;
        mode_ = SvfFilter::Mode::LowPass;
        mine_.SetMode(mode_);
        cutoff_hz_ = 20000.0f;
        resonance_ = 0.0f;
        ApplyConfig();
        Retune();
    }

    // Instrument-owned. A change retunes the incoming implementation (it has
    // not been tuned since it was last active, or ever) and starts it clean.
    // A value this build does not know falls back to the SVF.
    void SetTopology(FilterTopology topology) {
        if (static_cast<uint8_t>(topology) >= kFilterTopologyCount)
            topology = FilterTopology::WaveXSvf;
        if (topology == topology_)
            return;
        topology_ = topology;
        Retune();
        Reset();
    }
    FilterTopology GetTopology() const { return topology_; }

    void SetConfig(const FilterConfig& config) {
        if (config == config_)
            return;
        config_ = config;
        ApplyConfig();
    }
    const FilterConfig& GetConfig() const { return config_; }

    void SetMode(SvfFilter::Mode mode) {
        mode_ = static_cast<uint8_t>(mode) <= 3 ? mode : SvfFilter::Mode::LowPass;
        mine_.SetMode(mode_);
        ladder_.SetFilterMode(LadderMode());
    }
    SvfFilter::Mode GetMode() const { return mode_; }

    void SetCutoff(float hz) { SetParameters(hz, resonance_); }

    void SetResonance(float res) { SetParameters(cutoff_hz_, res); }
    float Resonance() const { return resonance_; }

    // One tuning pair produces one coefficient set. Keep the integrators
    // intact, just as the separate setters do during a sounding note.
    void SetParameters(float hz, float res) {
        hz = hz > 0.0f ? hz : 0.0f;
        res = res < 0.0f ? 0.0f : (res > 1.0f ? 1.0f : res);
        // A full live Instrument snapshot also arrives for oscillator, amp,
        // envelope and LFO edits. Those leave these coefficients unchanged.
        if (hz == cutoff_hz_ && res == resonance_)
            return;
        cutoff_hz_ = hz;
        resonance_ = res;
        Retune();
    }

    float Process(float in) {
        if (topology_ == FilterTopology::WaveXSvf) {
            return mine_.Process(in);
        }
        if (ladder_boundary_) {
            const bool pass =
                ladder_boundary_ == 2
                    ? mode_ == SvfFilter::Mode::LowPass || mode_ == SvfFilter::Mode::Notch
                    : mode_ == SvfFilter::Mode::HighPass || mode_ == SvfFilter::Mode::Notch;
            return pass ? in : 0.f;
        }
        const float out = ladder_.Process(in);
        return mode_ == SvfFilter::Mode::Notch ? in - out : out;
    }

    // Clears state, keeps tuning, mode and config.
    void Reset() {
        if (topology_ == FilterTopology::WaveXSvf) {
            mine_.Reset();
        } else {
            ladder_.Reset();
        }
    }

   private:
    // The ladder response for mode x slope. Notch runs the BP12 tap, which
    // Process() subtracts from the input.
    ZdfLadder::Mode LadderMode() const {
        using M = ZdfLadder::Mode;
        const bool db24 = config_.slope == SvfFilter::Slope::Db24;
        switch (mode_) {
            case SvfFilter::Mode::HighPass:
                return db24 ? M::HP24 : M::HP12;
            case SvfFilter::Mode::BandPass:
                return db24 ? M::BP24 : M::BP12;
            case SvfFilter::Mode::Notch:
                return M::BP12;
            default:
                return db24 ? M::LP24 : M::LP12;
        }
    }

    void ApplyConfig() {
        mine_.SetSlope(config_.slope);
        mine_.SetDrive(config_.drive);
        ladder_.SetFilterMode(LadderMode());
        // The ladder's input always runs through its saturation; its drive
        // is a gain of up to 4 into that stage. 0..1 onto 1..4 so drive 0
        // leaves the level alone - the same law the SVF's input stage uses.
        ladder_.SetInputDrive(1.0f + config_.drive * 3.0f);
    }

    void Retune() {
        if (topology_ == FilterTopology::WaveXSvf) {
            mine_.SetParameters(cutoff_hz_, resonance_);
            return;
        }
        const float nyquist = static_cast<float>(sample_rate_) * 0.5f;
        ladder_boundary_ = cutoff_hz_ >= nyquist ? 2 : cutoff_hz_ <= 0 ? 1 : 0;
        if (ladder_boundary_)
            return;
        ladder_.SetFreq(cutoff_hz_);
        ladder_.SetRes(resonance_ * kLadderResonanceScale);
    }

    // 100% = loop gain 5.4 (the ladder's own unit is loop gain / 4). 4.0 is
    // exactly marginal and the saturation damps it, so the scale is what
    // makes the top of the travel sing; oscillation starts at 4 / 5.4 = 74%.
    static constexpr float kLadderResonanceScale = 1.35f;

    uint32_t sample_rate_ = 48000;
    FilterConfig config_;
    FilterTopology topology_ = FilterTopology::WaveXSvf;
    float cutoff_hz_ = 20000.0f;
    float resonance_ = 0.0f;
    uint8_t ladder_boundary_ = 2;
    SvfFilter::Mode mode_ = SvfFilter::Mode::LowPass;

    SvfFilter mine_;
    ZdfLadder ladder_;
};

}  // namespace AudioEngine
}  // namespace WaveX
