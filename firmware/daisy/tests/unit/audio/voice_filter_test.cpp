// Unit tests for VoiceFilter (src/audio/voice_filter.hpp): the per-voice
// filter that each Instrument switches between the first-party TPT SVF and
// daisysp::LadderFilter. What is pinned is the contract the voice path
// relies on being the same whichever topology is selected - exact bypass at
// or above Nyquist, a real filter below it, callback-safe tuning while
// running, the four modes - plus that switching between them mid-note is
// clean and that the bench slope/drive reach both.

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
// The ladder's input stage is a tanh even at drive 0, so its response is
// measured well inside the linear region and reported relative to the
// input amplitude; the SVF is linear and gets the same treatment.
constexpr float kProbe = 0.2f;

// Steady-state peak of a sine through the filter, relative to its amplitude.
float SteadyStateGain(VoiceFilter& f, float hz, float amplitude = kProbe, size_t cycles = 200) {
    const size_t n = static_cast<size_t>(static_cast<float>(kSr) / hz * static_cast<float>(cycles));
    const size_t settle = n / 2;
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float in =
            amplitude * std::sin(2.0f * kPi * hz * static_cast<float>(i) / static_cast<float>(kSr));
        const float out = f.Process(in);
        if (i >= settle)
            peak = std::max(peak, std::fabs(out));
    }
    return peak / amplitude;
}

VoiceFilter MakeFilter(FilterTopology topology, float cutoff_hz, float res = 0.0f) {
    VoiceFilter f;
    f.Init(kSr);
    f.SetTopology(topology);
    f.SetResonance(res);
    f.SetCutoff(cutoff_hz);
    f.Reset();
    return f;
}

const FilterTopology kBoth[] = {FilterTopology::WaveXSvf, FilterTopology::Ladder};

}  // namespace

TEST(VoiceFilterTest, DefaultIsTheWaveXSvfAndPassesThroughItUnchanged) {
    // The default configuration must be the linear 12 dB WaveX filter, and
    // VoiceFilter must add nothing of its own to it: the voice-manager tests
    // were written against SvfFilter directly.
    VoiceFilter wrapped;
    wrapped.Init(kSr);
    EXPECT_EQ(wrapped.GetTopology(), FilterTopology::WaveXSvf);
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

TEST(VoiceFilterTest, UnknownTopologyFallsBackToTheSvf) {
    VoiceFilter f = MakeFilter(FilterTopology::Ladder, 1000.0f);
    ASSERT_EQ(f.GetTopology(), FilterTopology::Ladder);
    f.SetTopology(static_cast<FilterTopology>(7));
    EXPECT_EQ(f.GetTopology(), FilterTopology::WaveXSvf);
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
        EXPECT_NEAR(SteadyStateGain(f, 50.0f), 1.0f, 0.05f) << static_cast<int>(t);
        const float db = 20.0f * std::log10(SteadyStateGain(f, 8000.0f));
        EXPECT_LT(db, -20.0f) << "topology " << static_cast<int>(t) << " three octaves up";
    }
}

TEST(VoiceFilterTest, BothTopologiesResonate) {
    for (FilterTopology t: kBoth) {
        VoiceFilter flat = MakeFilter(t, 1000.0f, 0.0f);
        VoiceFilter peaky = MakeFilter(t, 1000.0f, 0.8f);
        EXPECT_GT(SteadyStateGain(peaky, 1000.0f), 1.5f * SteadyStateGain(flat, 1000.0f))
            << "topology " << static_cast<int>(t);
    }
}

TEST(VoiceFilterTest, TheLadderIsNotTheSvfInDisguise) {
    VoiceFilter svf = MakeFilter(FilterTopology::WaveXSvf, 1000.0f, 0.5f);
    VoiceFilter ladder = MakeFilter(FilterTopology::Ladder, 1000.0f, 0.5f);
    float difference = 0.0f;
    for (int i = 0; i < 4800; ++i) {
        const float in = kProbe * std::sin(2.0f * kPi * 700.0f * static_cast<float>(i) / 48000.0f);
        difference = std::max(difference, std::fabs(svf.Process(in) - ladder.Process(in)));
    }
    EXPECT_GT(difference, 0.01f);
}

TEST(VoiceFilterTest, SwitchingTopologyMidNoteStaysFiniteAndRetunes) {
    VoiceFilter f = MakeFilter(FilterTopology::WaveXSvf, 600.0f, 0.7f);
    float peak_after = 0.0f;
    for (int i = 0; i < 96000; ++i) {
        if (i == 48000) {
            f.SetTopology(FilterTopology::Ladder);  // the ladder had never been tuned until now
        }
        const float in = kProbe * std::sin(2.0f * kPi * 5000.0f * static_cast<float>(i) / 48000.0f);
        const float out = f.Process(in);
        ASSERT_TRUE(std::isfinite(out)) << "sample " << i;
        if (i > 72000)
            peak_after = std::max(peak_after, std::fabs(out));
    }
    // 5 kHz through a 600 Hz lowpass: the switched-in filter must actually be
    // filtering at the cutoff it inherited, not at DaisySP's Init() default.
    EXPECT_LT(peak_after / kProbe, 0.1f);
}

TEST(VoiceFilterTest, SwitchingTopologyStartsTheIncomingFilterClean) {
    // Charge the SVF to DC, switch to the ladder and back: neither switch may
    // hand the incoming implementation the other's charged state.
    VoiceFilter f = MakeFilter(FilterTopology::WaveXSvf, 500.0f);
    for (int i = 0; i < 2000; ++i)
        f.Process(1.0f);
    f.SetTopology(FilterTopology::Ladder);
    EXPECT_LT(std::fabs(f.Process(0.0f)), 1e-3f);
    for (int i = 0; i < 2000; ++i)
        f.Process(1.0f);
    f.SetTopology(FilterTopology::WaveXSvf);
    EXPECT_LT(std::fabs(f.Process(0.0f)), 1e-3f);
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

TEST(VoiceFilterTest, SlopeReachesBothTopologies) {
    for (FilterTopology t: kBoth) {
        VoiceFilter f12 = MakeFilter(t, 1000.0f);
        VoiceFilter f24 = MakeFilter(t, 1000.0f);
        FilterConfig cfg = f24.GetConfig();
        cfg.slope = SvfFilter::Slope::Db24;
        f24.SetConfig(cfg);
        EXPECT_LT(SteadyStateGain(f24, 4000.0f), 0.5f * SteadyStateGain(f12, 4000.0f))
            << "topology " << static_cast<int>(t);
    }
}

TEST(VoiceFilterTest, DriveReachesTheLadderWithoutChangingItsLevelAtZero) {
    // Drive 0 must not attenuate (DaisySP's own default halves the input),
    // and full drive must push the tanh harder: a loud sine comes out with
    // a different, flatter shape. (It does not come out quieter - the
    // saturated level is the tanh ceiling either way.)
    VoiceFilter clean = MakeFilter(FilterTopology::Ladder, 5000.0f);
    EXPECT_NEAR(SteadyStateGain(clean, 100.0f, 0.05f), 1.0f, 0.02f);
    VoiceFilter hot = MakeFilter(FilterTopology::Ladder, 5000.0f);
    FilterConfig cfg = hot.GetConfig();
    cfg.drive = 1.0f;
    hot.SetConfig(cfg);
    clean.Reset();
    float difference = 0.0f;
    for (int i = 0; i < 9600; ++i) {
        const float in = std::sin(2.0f * kPi * 100.0f * static_cast<float>(i) / 48000.0f);
        const float a = clean.Process(in);
        const float b = hot.Process(in);
        if (i > 4800)
            difference = std::max(difference, std::fabs(a - b));
    }
    EXPECT_GT(difference, 0.1f);
}

TEST(VoiceFilterTest, ResetKeepsTheLadderTunedShapedAndInMode) {
    // The ladder can only be cleared by re-initialising it, which also
    // resets its tuning: Reset() must put cutoff, mode, slope and drive back.
    VoiceFilter f = MakeFilter(FilterTopology::Ladder, 600.0f, 0.3f);
    f.SetMode(SvfFilter::Mode::HighPass);
    FilterConfig cfg = f.GetConfig();
    cfg.slope = SvfFilter::Slope::Db24;
    f.SetConfig(cfg);
    const float before_low = SteadyStateGain(f, 100.0f);
    const float before_high = SteadyStateGain(f, 6000.0f);
    f.Reset();
    EXPECT_EQ(f.GetMode(), SvfFilter::Mode::HighPass);
    EXPECT_EQ(f.GetTopology(), FilterTopology::Ladder);
    EXPECT_NEAR(SteadyStateGain(f, 100.0f), before_low, 1e-3f);
    EXPECT_NEAR(SteadyStateGain(f, 6000.0f), before_high, 1e-3f);
    EXPECT_LT(before_low, 0.05f);
    EXPECT_GT(before_high, 0.9f);
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
                    cfg.slope = slope;
                    cfg.drive = drive;
                    combined.SetConfig(cfg);
                    separate.SetConfig(cfg);
                    combined.SetTopology(topology);
                    separate.SetTopology(topology);
                    for (float cutoff: {1200.0f, 18000.0f, 1.0e6f, -20.0f, 700.0f}) {
                        for (float resonance: {0.7f, -0.1f, 1.2f, 0.0f}) {
                            combined.SetParameters(cutoff, resonance);
                            separate.SetResonance(resonance);
                            separate.SetCutoff(cutoff);
                            for (int phase = 0; phase < 3; ++phase) {
                                if (phase == 1) {
                                    const FilterTopology other =
                                        combined.GetTopology() == FilterTopology::WaveXSvf
                                            ? FilterTopology::Ladder
                                            : FilterTopology::WaveXSvf;
                                    combined.SetTopology(other);
                                    separate.SetTopology(other);
                                } else if (phase == 2) {
                                    combined.Reset();
                                    separate.Reset();
                                }
                                for (int i = 0; i < 96; ++i) {
                                    // Unrelated live sound edits repeat the filter snapshot.
                                    // They must leave the charged integrators and output intact.
                                    combined.SetConfig(cfg);
                                    combined.SetTopology(combined.GetTopology());
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

TEST(VoiceFilterTest, SvfModesRejectTheExpectedFrequencyBands) {
    using Mode = SvfFilter::Mode;
    for (Mode mode: {Mode::LowPass, Mode::HighPass, Mode::BandPass, Mode::Notch}) {
        auto f = MakeFilter(FilterTopology::WaveXSvf, 1000);
        f.SetMode(mode);
        const auto low = SteadyStateGain(f, 100);
        const auto center = SteadyStateGain(f, 1000);
        const auto high = SteadyStateGain(f, 8000);
        if (mode == Mode::LowPass) {
            EXPECT_GT(low, .9f);
            EXPECT_LT(high, .03f);
        }
        if (mode == Mode::HighPass) {
            EXPECT_LT(low, .03f);
            EXPECT_GT(high, .9f);
        }
        if (mode == Mode::BandPass) {
            EXPECT_GT(center, 3 * low);
            EXPECT_GT(center, 3 * high);
        }
        if (mode == Mode::Notch) {
            EXPECT_LT(center, .001f);
            EXPECT_GT(low, .9f);
            EXPECT_GT(high, .9f);
        }
    }
}

TEST(VoiceFilterTest, LadderModesRejectTheExpectedFrequencyBands) {
    // The ladder's HP/BP are Huovilainen's weighted stage sums and its
    // Notch is input minus the 12 dB band-pass tap, so the skirts are
    // shallower and the HP passband settles later than the SVF's (its HP12
    // is still about -2 dB three octaves up); the bands still have to be
    // the right ones.
    using Mode = SvfFilter::Mode;
    for (Mode mode: {Mode::LowPass, Mode::HighPass, Mode::BandPass, Mode::Notch}) {
        auto f = MakeFilter(FilterTopology::Ladder, 1000);
        f.SetMode(mode);
        const auto low = SteadyStateGain(f, 100);
        const auto center = SteadyStateGain(f, 1000);
        const auto high = SteadyStateGain(f, 8000);
        if (mode == Mode::LowPass) {
            EXPECT_GT(low, .9f);
            EXPECT_LT(high, .05f);
        }
        if (mode == Mode::HighPass) {
            EXPECT_LT(low, .05f);
            EXPECT_GT(high, .7f);
        }
        if (mode == Mode::BandPass) {
            EXPECT_GT(center, 3 * low);
            EXPECT_GT(center, 3 * high);
        }
        if (mode == Mode::Notch) {
            EXPECT_LT(center, .1f);
            EXPECT_GT(low, .9f);
            EXPECT_GT(high, .9f);
        }
    }
}

TEST(VoiceFilterTest, ModeCutoffBoundariesAndResonantDriveStayFinite) {
    using Mode = SvfFilter::Mode;
    for (auto topology: kBoth) {
        for (auto mode: {Mode::LowPass, Mode::HighPass, Mode::BandPass, Mode::Notch}) {
            auto f = MakeFilter(topology, 1000, 1);
            f.SetMode(mode);
            auto cfg = f.GetConfig();
            cfg.slope = SvfFilter::Slope::Db24;
            cfg.drive = 1;
            f.SetConfig(cfg);
            for (int i = 0; i < 16000; ++i) {
                if (i % 48 == 0)
                    f.SetParameters(20.f + (i % 240) * 99.f, 1);
                const float out = f.Process(std::sin(i * .1f));
                ASSERT_TRUE(std::isfinite(out));
                ASSERT_LT(std::fabs(out), 1000.f);
            }
            for (float hz: {0.f, 24000.f, 96000.f}) {
                f.SetCutoff(hz);
                const bool pass = hz == 0 ? mode == Mode::HighPass || mode == Mode::Notch
                                          : mode == Mode::LowPass || mode == Mode::Notch;
                EXPECT_FLOAT_EQ(f.Process(.37f), pass ? .37f : 0);
            }
        }
    }
}
