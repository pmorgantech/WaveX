// Unit tests for VoiceFilter (src/audio/voice_filter.hpp): the per-voice
// lowpass that can be switched between the first-party TPT SVF and
// daisysp::Svf. What is pinned is the contract the voice path relies on
// being the same whichever implementation is selected - exact bypass at or
// above Nyquist, a real lowpass below it, callback-safe tuning while
// running - plus that switching between them mid-note is clean.

#include "audio/voice_filter.hpp"

#include <gtest/gtest.h>

#include <cmath>

using WaveX::AudioEngine::FilterConfig;
using WaveX::AudioEngine::FilterTopology;
using WaveX::AudioEngine::SvfFilter;
using WaveX::AudioEngine::VoiceFilter;

namespace {

constexpr uint32_t kSr = 48000;
constexpr float kPi = 3.14159265358979323846f;

float SteadyStatePeak(VoiceFilter& f, float hz, size_t cycles = 200) {
    const size_t n = static_cast<size_t>(static_cast<float>(kSr) / hz * static_cast<float>(cycles));
    const size_t settle = n / 2;
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float in =
            std::sin(2.0f * kPi * hz * static_cast<float>(i) / static_cast<float>(kSr));
        const float out = f.Process(in);
        if (i >= settle)
            peak = std::max(peak, std::fabs(out));
    }
    return peak;
}

VoiceFilter MakeFilter(FilterTopology topology, float cutoff_hz, float res = 0.0f) {
    VoiceFilter f;
    f.Init(kSr);
    FilterConfig cfg;
    cfg.topology = topology;
    f.SetConfig(cfg);
    f.SetResonance(res);
    f.SetCutoff(cutoff_hz);
    f.Reset();
    return f;
}

const FilterTopology kBoth[] = {FilterTopology::WaveXSvf, FilterTopology::DaisySpSvf};

}  // namespace

TEST(VoiceFilterTest, DefaultIsTheWaveXSvfAndPassesThroughItUnchanged) {
    // The default configuration must be the linear 12 dB WaveX filter, and
    // VoiceFilter must add nothing of its own to it: the voice-manager tests
    // were written against SvfFilter directly.
    VoiceFilter wrapped;
    wrapped.Init(kSr);
    EXPECT_EQ(wrapped.GetConfig().topology, FilterTopology::WaveXSvf);
    EXPECT_EQ(wrapped.GetConfig().slope, SvfFilter::Slope::Db12);
    EXPECT_FLOAT_EQ(wrapped.GetConfig().drive, 0.0f);

    wrapped.SetResonance(0.6f);
    wrapped.SetCutoff(800.0f);
    wrapped.Reset();
    SvfFilter bare;
    bare.Init(kSr);
    bare.SetResonance(0.6f);
    bare.SetCutoff(800.0f);
    bare.Reset();
    for (int i = 0; i < 2000; ++i) {
        const float in = std::sin(2.0f * kPi * 700.0f * static_cast<float>(i) / 48000.0f);
        ASSERT_FLOAT_EQ(wrapped.Process(in), bare.Process(in)) << "sample " << i;
    }
}

TEST(VoiceFilterTest, BothTopologiesBypassExactlyAtOrAboveNyquist) {
    for (FilterTopology t: kBoth) {
        VoiceFilter f = MakeFilter(t, 1.0e6f, 1.0f);
        for (float x: {1.0f, -1.0f, 0.5f, -0.25f, 0.0f, 0.99f}) {
            EXPECT_FLOAT_EQ(f.Process(x), x) << "topology " << static_cast<int>(t);
        }
        VoiceFilter g = MakeFilter(t, static_cast<float>(kSr) * 0.5f);
        EXPECT_FLOAT_EQ(g.Process(0.7f), 0.7f);
    }
}

TEST(VoiceFilterTest, BothTopologiesAreLowpassesBelowNyquist) {
    for (FilterTopology t: kBoth) {
        VoiceFilter f = MakeFilter(t, 1000.0f);
        EXPECT_NEAR(SteadyStatePeak(f, 50.0f), 1.0f, 0.05f) << static_cast<int>(t);
        const float db = 20.0f * std::log10(SteadyStatePeak(f, 8000.0f));
        EXPECT_LT(db, -20.0f) << "topology " << static_cast<int>(t) << " three octaves up";
    }
}

TEST(VoiceFilterTest, BothTopologiesResonate) {
    for (FilterTopology t: kBoth) {
        VoiceFilter flat = MakeFilter(t, 1000.0f, 0.0f);
        VoiceFilter peaky = MakeFilter(t, 1000.0f, 0.8f);
        EXPECT_GT(SteadyStatePeak(peaky, 1000.0f), 1.5f * SteadyStatePeak(flat, 1000.0f))
            << "topology " << static_cast<int>(t);
    }
}

TEST(VoiceFilterTest, SwitchingTopologyMidNoteStaysFiniteAndRetunes) {
    VoiceFilter f = MakeFilter(FilterTopology::WaveXSvf, 600.0f, 0.7f);
    float peak_after = 0.0f;
    for (int i = 0; i < 96000; ++i) {
        if (i == 48000) {
            FilterConfig cfg = f.GetConfig();
            cfg.topology = FilterTopology::DaisySpSvf;
            f.SetConfig(cfg);  // the DaisySP side had never been tuned until now
        }
        const float in = std::sin(2.0f * kPi * 5000.0f * static_cast<float>(i) / 48000.0f);
        const float out = f.Process(in);
        ASSERT_TRUE(std::isfinite(out)) << "sample " << i;
        if (i > 72000)
            peak_after = std::max(peak_after, std::fabs(out));
    }
    // 5 kHz through a 600 Hz lowpass: the switched-in filter must actually be
    // filtering at the cutoff it inherited, not at DaisySP's Init() default.
    EXPECT_LT(peak_after, 0.1f);
}

TEST(VoiceFilterTest, ConfigChangesWithinATopologyDoNotResetState) {
    // Slope/drive edits on a sounding voice must not click: only a topology
    // switch clears the integrators.
    VoiceFilter f = MakeFilter(FilterTopology::WaveXSvf, 500.0f);
    for (int i = 0; i < 1000; ++i)
        f.Process(1.0f);  // charge to DC
    FilterConfig cfg = f.GetConfig();
    cfg.drive = 0.5f;
    f.SetConfig(cfg);
    EXPECT_GT(f.Process(1.0f), 0.9f) << "state kept: still sitting at the DC level";
}

TEST(VoiceFilterTest, SlopeAndDriveReachTheWaveXSvf) {
    VoiceFilter f12 = MakeFilter(FilterTopology::WaveXSvf, 1000.0f);
    VoiceFilter f24 = MakeFilter(FilterTopology::WaveXSvf, 1000.0f);
    FilterConfig cfg = f24.GetConfig();
    cfg.slope = SvfFilter::Slope::Db24;
    f24.SetConfig(cfg);
    EXPECT_LT(SteadyStatePeak(f24, 4000.0f), 0.5f * SteadyStatePeak(f12, 4000.0f));
}

TEST(VoiceFilterTest, CombinedTuningPreservesSeparateSetterOutputAndState) {
    // Exercise tuning on charged integrators, including clamping, bypass,
    // reset and topology changes. A combined update must preserve the sound.
    for (uint32_t sample_rate: {44100u, 48000u, 96000u}) {
        for (FilterTopology topology: kBoth) {
            for (SvfFilter::Slope slope: {SvfFilter::Slope::Db12, SvfFilter::Slope::Db24}) {
                for (float drive: {0.0f, 1.0f}) {
                    VoiceFilter combined, separate;
                    combined.Init(sample_rate);
                    separate.Init(sample_rate);
                    FilterConfig cfg;
                    cfg.topology = topology;
                    cfg.slope = slope;
                    cfg.drive = drive;
                    combined.SetConfig(cfg);
                    separate.SetConfig(cfg);
                    for (float cutoff: {1200.0f, 18000.0f, 1.0e6f, -20.0f, 700.0f}) {
                        for (float resonance: {0.7f, -0.1f, 1.2f, 0.0f}) {
                            combined.SetParameters(cutoff, resonance);
                            separate.SetResonance(resonance);
                            separate.SetCutoff(cutoff);
                            for (int phase = 0; phase < 3; ++phase) {
                                if (phase == 1) {
                                    cfg.topology = cfg.topology == FilterTopology::WaveXSvf
                                                       ? FilterTopology::DaisySpSvf
                                                       : FilterTopology::WaveXSvf;
                                    combined.SetConfig(cfg);
                                    separate.SetConfig(cfg);
                                } else if (phase == 2) {
                                    combined.Reset();
                                    separate.Reset();
                                }
                                for (int i = 0; i < 96; ++i) {
                                    // Unrelated live sound edits repeat the filter snapshot.
                                    // They must leave the charged integrators and output intact.
                                    combined.SetConfig(cfg);
                                    combined.SetParameters(cutoff, resonance);
                                    const float input =
                                        0.2f *
                                        std::sin(2.0f * kPi * 700.0f * static_cast<float>(i) /
                                                 static_cast<float>(sample_rate));
                                    ASSERT_FLOAT_EQ(combined.Process(input),
                                                    separate.Process(input))
                                        << sample_rate << " Hz; cutoff " << cutoff << "; resonance "
                                        << resonance << "; phase " << phase;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
