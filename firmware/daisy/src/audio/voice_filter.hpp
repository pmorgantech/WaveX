#pragma once

// The per-voice lowpass, selectable between two implementations so they can
// be compared by ear on the same material without reflashing:
//
//   FilterTopology::WaveXSvf   - svf_filter.hpp: the first-party TPT SVF,
//                                with its 12/24 dB slope and soft-clip drive.
//   FilterTopology::DaisySpSvf - daisysp::Svf: Andrew Simper's double-sampled
//                                Chamberlin SVF as shipped in DaisySP, lowpass
//                                output, with its own cubic drive term on the
//                                bandpass state. About 3x the per-sample cost
//                                of the 12 dB WaveX stage (two passes and
//                                five outputs computed per sample).
//
// FilterConfig is the whole selectable surface. It is NOT on the inter-MCU
// wire or in the UI: the debug console's "WAVEX-FILTER" command (main.cpp)
// publishes it through the voice-live mailbox, which is enough for an A/B
// listen. Promoting any of it to a real parameter is a protocol change and
// is backlogged as such.
//
// Contracts kept identical across both:
//   - cutoff at or above Nyquist is an EXACT bypass (the voice-manager tests
//     rely on it; daisysp::Svf would otherwise clamp to sr/3 and still
//     filter), so it is handled here before either implementation sees it;
//   - resonance 0..1, cutoff in Hz, both callable at block rate from the
//     callback: no allocation, only the tuning transcendentals each already
//     pays (tan() here, sinf()+powf() in DaisySP) and only when tuning changes.
//
// Only the ACTIVE implementation is retuned on SetCutoff/SetResonance; the
// other is brought up to date when it is switched in, so an idle topology
// costs nothing per block.
//
// Both filters live inside Voice, so they inherit VoiceManager's DTCM
// placement (see svf_filter.hpp on why that matters).
//
// HAL-free: host tests compile this with DaisySP's svf.cpp.

#include "Filters/svf.h"

#include "svf_filter.hpp"
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

enum class FilterTopology : uint8_t { WaveXSvf = 0, DaisySpSvf = 1 };

struct FilterConfig {
    FilterTopology topology = FilterTopology::WaveXSvf;
    SvfFilter::Slope slope = SvfFilter::Slope::Db12;  // WaveXSvf only
    float drive = 0.0f;                               // 0..1; WaveXSvf soft clip, DaisySP SetDrive

    bool operator==(const FilterConfig& o) const {
        return topology == o.topology && slope == o.slope && drive == o.drive;
    }
    bool operator!=(const FilterConfig& o) const { return !(*this == o); }
};

class VoiceFilter {
   public:
    void Init(uint32_t sample_rate) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        mine_.Init(sample_rate_);
        dsp_.Init(static_cast<float>(sample_rate_));
        config_ = FilterConfig{};
        cutoff_hz_ = 20000.0f;
        resonance_ = 0.0f;
        ApplyConfig();
        Retune();
    }

    void SetConfig(const FilterConfig& config) {
        const bool switching = config.topology != config_.topology;
        config_ = config;
        ApplyConfig();
        if (switching) {
            // The incoming implementation has not been tuned since it was
            // last active (or ever); bring it up to date and start it clean
            // so the previous topology's state cannot leak through as a
            // click.
            Retune();
            Reset();
        }
    }
    const FilterConfig& GetConfig() const { return config_; }

    void SetCutoff(float hz) {
        cutoff_hz_ = hz > 0.0f ? hz : 0.0f;
        Retune();
    }

    void SetResonance(float res) {
        resonance_ = res < 0.0f ? 0.0f : (res > 1.0f ? 1.0f : res);
        Retune();
    }

    float Process(float in) {
        if (config_.topology == FilterTopology::WaveXSvf) {
            return mine_.Process(in);
        }
        if (dsp_bypass_) {
            return in;
        }
        dsp_.Process(in);
        return dsp_.Low();
    }

    // Clears state, keeps tuning and config.
    void Reset() {
        if (config_.topology == FilterTopology::WaveXSvf) {
            mine_.Reset();
        } else {
            // daisysp::Svf has no state reset; Init() is the only way to
            // zero its integrators, and it also resets the tuning.
            dsp_.Init(static_cast<float>(sample_rate_));
            TuneDsp();
            dsp_.SetDrive(config_.drive);
        }
    }

   private:
    void ApplyConfig() {
        mine_.SetSlope(config_.slope);
        mine_.SetDrive(config_.drive);
        dsp_.SetDrive(config_.drive);
    }

    void Retune() {
        if (config_.topology == FilterTopology::WaveXSvf) {
            mine_.SetCutoff(cutoff_hz_);
            mine_.SetResonance(resonance_);
        } else {
            TuneDsp();
        }
    }

    void TuneDsp() {
        const float nyquist = static_cast<float>(sample_rate_) * 0.5f;
        dsp_bypass_ = cutoff_hz_ >= nyquist;
        if (!dsp_bypass_) {
            dsp_.SetFreq(cutoff_hz_);
            dsp_.SetRes(resonance_);
        }
    }

    uint32_t sample_rate_ = 48000;
    FilterConfig config_;
    float cutoff_hz_ = 20000.0f;
    float resonance_ = 0.0f;
    bool dsp_bypass_ = true;

    SvfFilter mine_;
    daisysp::Svf dsp_;
};

}  // namespace AudioEngine
}  // namespace WaveX
