// Unit tests for ZdfLadder (src/audio/ladder_zdf.hpp): a 4-pole lowpass with
// exact tuning, a resonance that peaks at the cutoff and self-oscillates at
// the top without blowing up, the six responses in their bands, and a linear
// resonance range that reaches self-oscillation cleanly.

#include "audio/ladder_zdf.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using WaveX::AudioEngine::ZdfLadder;

namespace {

constexpr float kSr = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;

// Small-signal by default: the loop clipper starts to bite well below a
// 0.2 probe once resonance multiplies it, and tuning is a linear property.
template <typename F>
float SteadyStateGain(F& f, float hz, float amplitude = 0.02f, size_t cycles = 200) {
    const size_t n = static_cast<size_t>(kSr / hz * static_cast<float>(cycles));
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float in = amplitude * std::sin(2.0f * kPi * hz * static_cast<float>(i) / kSr);
        const float out = f.Process(in);
        if (i >= n / 2)
            peak = std::max(peak, std::fabs(out));
    }
    return peak / amplitude;
}

ZdfLadder Make(float cutoff, float res, ZdfLadder::Mode mode = ZdfLadder::Mode::LP24) {
    ZdfLadder f;
    f.Init(kSr);
    f.SetFilterMode(mode);
    f.SetRes(res);
    f.SetFreq(cutoff);
    f.Reset();
    return f;
}

}  // namespace

TEST(ZdfLadderTest, IsAFourPoleLowpassWithExactTuning) {
    for (float cutoff: {100.0f, 1000.0f, 6000.0f}) {
        ZdfLadder f = Make(cutoff, 0.0f);
        EXPECT_NEAR(SteadyStateGain(f, cutoff * 0.05f), 1.0f, 0.03f) << cutoff;
        f.Reset();
        // Four identical one-poles: -3 dB each at the cutoff, -12 dB in all.
        EXPECT_NEAR(SteadyStateGain(f, cutoff), 0.25f, 0.03f) << cutoff;
        f.Reset();
        const float db = 20.0f * std::log10(SteadyStateGain(f, cutoff * 4.0f));
        EXPECT_LT(db, -40.0f) << cutoff;
    }
}

TEST(ZdfLadderTest, ResonancePeaksAtTheCutoff) {
    ZdfLadder flat = Make(1000.0f, 0.0f);
    ZdfLadder peaky = Make(1000.0f, 0.8f);
    const float at = SteadyStateGain(peaky, 1000.0f);
    EXPECT_GT(at, 2.0f * SteadyStateGain(flat, 1000.0f));
    peaky.Reset();
    EXPECT_GT(at, SteadyStateGain(peaky, 700.0f));
    peaky.Reset();
    EXPECT_GT(at, SteadyStateGain(peaky, 1400.0f));
}

TEST(ZdfLadderTest, SelfOscillatesAboveUnityResonanceNearTheCutoffAndStaysBounded) {
    ZdfLadder f = Make(440.0f, 1.1f);
    f.Process(0.5f);  // one kick, then silence
    float peak = 0.0f;
    int crossings = 0;
    float last = 0.0f;
    const int start = 48000, n = 96000;  // measure the second second
    for (int i = 0; i < n; ++i) {
        const float out = f.Process(0.0f);
        ASSERT_TRUE(std::isfinite(out));
        ASSERT_LT(std::fabs(out), 10.0f);
        if (i >= start) {
            peak = std::max(peak, std::fabs(out));
            if ((last < 0.0f) != (out < 0.0f))
                ++crossings;
            last = out;
        }
    }
    EXPECT_GT(peak, 0.05f) << "died away";
    EXPECT_LT(peak, 2.0f) << "the clip bounds it";
    const float hz = static_cast<float>(crossings) / 2.0f;
    EXPECT_NEAR(hz, 440.0f, 440.0f * 0.1f);
}

TEST(ZdfLadderTest, ModesRejectTheExpectedBands) {
    using Mode = ZdfLadder::Mode;
    for (Mode mode: {Mode::LP24, Mode::LP12, Mode::BP24, Mode::BP12, Mode::HP24, Mode::HP12}) {
        ZdfLadder f = Make(1000.0f, 0.0f, mode);
        const float low = SteadyStateGain(f, 100.0f);
        f.Reset();
        const float center = SteadyStateGain(f, 1000.0f);
        f.Reset();
        const float high = SteadyStateGain(f, 10000.0f);
        switch (mode) {
            case Mode::LP24:
            case Mode::LP12:
                EXPECT_GT(low, 0.9f) << static_cast<int>(mode);
                EXPECT_LT(high, 0.05f) << static_cast<int>(mode);
                break;
            case Mode::HP24:
            case Mode::HP12:
                EXPECT_LT(low, 0.05f) << static_cast<int>(mode);
                EXPECT_GT(high, 0.9f) << static_cast<int>(mode);
                break;
            default:
                EXPECT_GT(center, 3.0f * low) << static_cast<int>(mode);
                EXPECT_GT(center, 3.0f * high) << static_cast<int>(mode);
        }
    }
}

TEST(ZdfLadderTest, DriveIsAGainIntoTheClipWithNoMakeUp) {
    // Drive 1 is unity; drive 4 is DaisySP's 2.5x (passband-weighted) into
    // the clip with no make-up - the same law the SVF's input stage uses.
    ZdfLadder soft = Make(5000.0f, 0.0f);
    ZdfLadder hot = Make(5000.0f, 0.0f);
    hot.SetInputDrive(4.0f);
    EXPECT_NEAR(SteadyStateGain(soft, 100.0f, 0.05f), 1.0f, 0.02f);
    EXPECT_NEAR(SteadyStateGain(hot, 100.0f, 0.01f), 2.5f, 0.1f);
}

TEST(ZdfLadderTest, SweepsAtFullResonanceAndDriveStayFinite) {
    using Mode = ZdfLadder::Mode;
    for (Mode mode: {Mode::LP24, Mode::LP12, Mode::BP24, Mode::BP12, Mode::HP24, Mode::HP12}) {
        ZdfLadder f = Make(1000.0f, 1.0f, mode);
        f.SetInputDrive(4.0f);
        for (int i = 0; i < 24000; ++i) {
            if (i % 48 == 0)
                f.SetFreq(20.0f + static_cast<float>(i % 240) * 90.0f);
            const float out = f.Process(std::sin(static_cast<float>(i) * 0.1f));
            ASSERT_TRUE(std::isfinite(out)) << static_cast<int>(mode) << " " << i;
            ASSERT_LT(std::fabs(out), 100.0f);
        }
    }
}

TEST(ZdfLadderTest, ResetClearsStateAndKeepsTuning) {
    ZdfLadder f = Make(600.0f, 0.4f, ZdfLadder::Mode::HP12);
    ZdfLadder fresh = f;
    for (int i = 0; i < 2000; ++i)
        f.Process(1.0f);
    f.Reset();
    EXPECT_EQ(f.GetFilterMode(), ZdfLadder::Mode::HP12);
    for (int i = 0; i < 500; ++i) {
        const float in = 0.3f * std::sin(0.05f * static_cast<float>(i));
        ASSERT_FLOAT_EQ(f.Process(in), fresh.Process(in)) << i;
    }
}
