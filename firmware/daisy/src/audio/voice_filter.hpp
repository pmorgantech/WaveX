#pragma once

// The per-voice filter, selectable per Instrument between four topologies
// (Protocol::InstFilterTopology, carried by InstrumentFilter::topology):
//
//   FilterTopology::WaveXSvf   - svf_filter.hpp: the first-party TPT
//                                state-variable filter. LP/HP/BP/Notch, 12
//                                or 24 dB, cubic soft-clip drive.
//   FilterTopology::Ladder     - ladder_huovilainen.hpp at 4x oversampling:
//                                WaveX's port of DaisySP's port of the Teensy
//                                Huovilainen Moog ladder (all MIT), bit for
//                                bit the DaisySP filter. Four one-poles in a
//                                tanh feedback loop, LP/BP/HP at 12 or 24 dB
//                                with passband-gain compensation. About 500
//                                cycles per sample.
//   FilterTopology::LadderLite - the same port at 2x, Huovilainen's own
//                                published oversampling. Half the cost;
//                                nothing else differs, so an A/B against
//                                Ladder hears only the oversampling.
//   FilterTopology::LadderZdf  - ladder_zdf.hpp: a first-party zero-delay-
//                                feedback ladder, exact tuning with no
//                                oversampling and one saturation per
//                                sample. About the cost of the 24 dB SVF.
//
// All three ladders share DaisySP's response set (LP/BP/HP at 12 or 24 dB)
// and the same saturation curve. Notch is not a ladder response; it is
// rendered as input minus the 12 dB band-pass tap, a true null at the
// cutoff (that tap sits at unity gain and zero phase there) and only an
// approximation of a notch skirt away from it.
//
// Topology, mode, slope and drive are Instrument-owned voice character:
// they arrive on VoiceTriggerParams for the next note and through
// VoiceInstrumentParams for held ones. FilterConfig (slope, drive) applies
// to whichever topology is active.
//
// Contracts kept identical across all four:
//   - cutoff at or above Nyquist is an EXACT bypass for LP/Notch and silence
//     for HP/BP, and cutoff at or below zero the reverse (the voice-manager
//     tests rely on it; the ladders would otherwise clamp to their top and
//     still filter), so it is handled here before any of them sees it;
//   - resonance 0..1, cutoff in Hz, both callable at block rate from the
//     callback: no allocation, only the tuning arithmetic each already pays
//     (fast_tan.hpp's polynomial, or the Huovilainen tuning polynomials) and
//     only when the tuning changes.
//
// Only the ACTIVE topology is retuned on SetCutoff/SetResonance; the others
// are brought up to date when switched in, so an idle topology costs
// nothing per block. A switch clears the incoming filter's state so the
// previous topology cannot leak through as a click.
//
// COST. Measured 2026-09-14 on the profiling image, eight held modulated
// voices, 24 dB, full drive: SVF peak 30%, Ladder peak 70% of the block
// budget. LadderLite and LadderZdf are the candidates to bring the ladder
// sound under the gate; their rows are the open item in docs/roadmap.md
// "Outstanding hardware verification". Four topologies are the A/B set, not
// the product: the losers are removed once listened to.
//
// All four live inside Voice, so they inherit VoiceManager's DTCM placement
// (see svf_filter.hpp on why that matters). Each Voice holds every state
// (about 310 B in all); folding the inactive ones into a union is the
// obvious follow-up once the set is pruned.
//
// HAL-free: plain float arithmetic, host-testable.

#include "ladder_huovilainen.hpp"
#include "ladder_zdf.hpp"
#include "svf_filter.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

// Values match Protocol::InstFilterTopology (static_asserted in instrument.hpp).
enum class FilterTopology : uint8_t { WaveXSvf = 0, Ladder = 1, LadderLite = 2, LadderZdf = 3 };
static constexpr uint8_t kFilterTopologyCount = 4;

// Slope and drive. Both default OFF so a filter nobody configured is the
// linear 12 dB one the tests were written against.
struct FilterConfig {
    SvfFilter::Slope slope = SvfFilter::Slope::Db12;
    float drive = 0.0f;  // 0..1; SVF soft clip, ladders' input drive

    bool operator==(const FilterConfig& o) const { return slope == o.slope && drive == o.drive; }
    bool operator!=(const FilterConfig& o) const { return !(*this == o); }
};

class VoiceFilter {
    using LadderHq = HuovilainenLadder<4>;
    using LadderLite = HuovilainenLadder<2>;

   public:
    void Init(uint32_t sample_rate) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        mine_.Init(sample_rate_);
        ladder_hq_.Init(static_cast<float>(sample_rate_));
        ladder_lite_.Init(static_cast<float>(sample_rate_));
        ladder_zdf_.Init(static_cast<float>(sample_rate_));
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
        ApplyLadderMode();
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
        float out;
        switch (topology_) {
            case FilterTopology::LadderLite:
                out = ladder_lite_.Process(in);
                break;
            case FilterTopology::LadderZdf:
                out = ladder_zdf_.Process(in);
                break;
            default:
                out = ladder_hq_.Process(in);
                break;
        }
        return mode_ == SvfFilter::Mode::Notch ? in - out : out;
    }

    // Clears state, keeps tuning, mode and config.
    void Reset() {
        switch (topology_) {
            case FilterTopology::WaveXSvf:
                mine_.Reset();
                break;
            case FilterTopology::LadderLite:
                ladder_lite_.Reset();
                break;
            case FilterTopology::LadderZdf:
                ladder_zdf_.Reset();
                break;
            default:
                ladder_hq_.Reset();
                break;
        }
    }

   private:
    // The ladders share one response enumeration order (LP24, LP12, BP24,
    // BP12, HP24, HP12); this is the index of the response for mode x
    // slope. Notch runs the BP12 tap, which Process() subtracts from the
    // input.
    uint8_t LadderModeIndex() const {
        const bool db24 = config_.slope == SvfFilter::Slope::Db24;
        switch (mode_) {
            case SvfFilter::Mode::HighPass:
                return db24 ? 4 : 5;
            case SvfFilter::Mode::BandPass:
                return db24 ? 2 : 3;
            case SvfFilter::Mode::Notch:
                return 3;
            default:
                return db24 ? 0 : 1;
        }
    }
    template <typename L>
    void SetLadderMode(L& ladder) const {
        ladder.SetFilterMode(static_cast<typename L::Mode>(LadderModeIndex()));
    }
    void ApplyLadderMode() {
        SetLadderMode(ladder_hq_);
        SetLadderMode(ladder_lite_);
        SetLadderMode(ladder_zdf_);
    }

    void ApplyConfig() {
        mine_.SetSlope(config_.slope);
        mine_.SetDrive(config_.drive);
        ApplyLadderMode();
        ApplyLadderDrive();
    }

    // A ladder's input always runs through its saturation; its "drive" is a
    // gain of up to 4 into that stage. Map 0..1 onto 1..4 so drive 0 leaves
    // the level alone. The Huovilainen ladders also take DaisySP's
    // passband-gain compensation; the ZDF ladder applies its own.
    void ApplyLadderDrive() {
        const float drive = 1.0f + config_.drive * 3.0f;
        ladder_hq_.SetPassbandGain(kLadderPassbandGain);
        ladder_hq_.SetInputDrive(drive);
        ladder_lite_.SetPassbandGain(kLadderPassbandGain);
        ladder_lite_.SetInputDrive(drive);
        ladder_zdf_.SetInputDrive(drive);
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
        // Resonance 0..1 onto each ladder's 0..1 (loop gain 0..4): the
        // classic range. None of the topologies self-oscillates at 1; the
        // ladders' higher ceilings are not exposed.
        switch (topology_) {
            case FilterTopology::LadderLite:
                ladder_lite_.SetFreq(cutoff_hz_);
                ladder_lite_.SetRes(resonance_);
                break;
            case FilterTopology::LadderZdf:
                ladder_zdf_.SetFreq(cutoff_hz_);
                ladder_zdf_.SetRes(resonance_);
                break;
            default:
                ladder_hq_.SetFreq(cutoff_hz_);
                ladder_hq_.SetRes(resonance_);
                break;
        }
    }

    static constexpr float kLadderPassbandGain = 0.5f;

    uint32_t sample_rate_ = 48000;
    FilterConfig config_;
    FilterTopology topology_ = FilterTopology::WaveXSvf;
    float cutoff_hz_ = 20000.0f;
    float resonance_ = 0.0f;
    uint8_t ladder_boundary_ = 2;
    SvfFilter::Mode mode_ = SvfFilter::Mode::LowPass;

    SvfFilter mine_;
    LadderHq ladder_hq_;
    LadderLite ladder_lite_;
    ZdfLadder ladder_zdf_;
};

}  // namespace AudioEngine
}  // namespace WaveX
