// Pins the first-party Huovilainen ladder port (src/audio/ladder_huovilainen.hpp)
// against the vendored daisysp::LadderFilter it was ported from: the 4x
// variant must be arithmetic-for-arithmetic the same filter, so that an A/B
// against the 2x variant hears only the oversampling.

#include "audio/ladder_huovilainen.hpp"

#include <gtest/gtest.h>

#include "Filters/ladder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using WaveX::AudioEngine::HuovilainenLadder;

namespace {

constexpr float kSr = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;

using Reference = daisysp::LadderFilter;
using Ported = HuovilainenLadder<4>;
using Lite = HuovilainenLadder<2>;

daisysp::LadderFilter::FilterMode RefMode(Ported::Mode m) {
    using R = daisysp::LadderFilter::FilterMode;
    switch (m) {
        case Ported::Mode::LP24:
            return R::LP24;
        case Ported::Mode::LP12:
            return R::LP12;
        case Ported::Mode::BP24:
            return R::BP24;
        case Ported::Mode::BP12:
            return R::BP12;
        case Ported::Mode::HP24:
            return R::HP24;
        default:
            return R::HP12;
    }
}

template <typename F>
float SteadyStateGain(F& f, float hz, float amplitude = 0.2f, size_t cycles = 200) {
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

bool SameBits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

}  // namespace

TEST(HuovilainenLadderTest, FourTimesVariantIsBitIdenticalToDaisySp) {
    const Ported::Mode modes[] = {Ported::Mode::LP24,
                                  Ported::Mode::LP12,
                                  Ported::Mode::BP24,
                                  Ported::Mode::BP12,
                                  Ported::Mode::HP24,
                                  Ported::Mode::HP12};
    for (Ported::Mode mode: modes) {
        for (float cutoff: {60.0f, 800.0f, 5000.0f, 18000.0f, 30000.0f}) {
            for (float res: {0.0f, 0.6f, 1.0f, 1.8f}) {
                for (float drive: {0.5f, 1.0f, 4.0f}) {
                    Reference ref;
                    Ported mine;
                    ref.Init(kSr);
                    mine.Init(kSr);
                    ref.SetFilterMode(RefMode(mode));
                    mine.SetFilterMode(mode);
                    ref.SetPassbandGain(0.5f);
                    mine.SetPassbandGain(0.5f);
                    ref.SetInputDrive(drive);
                    mine.SetInputDrive(drive);
                    ref.SetFreq(cutoff);
                    mine.SetFreq(cutoff);
                    ref.SetRes(res);
                    mine.SetRes(res);
                    for (int i = 0; i < 4000; ++i) {
                        // Loud and broadband enough to exercise the tanh and
                        // every stage: a sine plus a square-ish harmonic.
                        const float t = static_cast<float>(i) / kSr;
                        const float in = 0.8f * std::sin(2.0f * kPi * 220.0f * t) +
                                         0.4f * (std::sin(2.0f * kPi * 3300.0f * t) > 0 ? 1 : -1);
                        const float a = ref.Process(in);
                        const float b = mine.Process(in);
                        ASSERT_TRUE(SameBits(a, b))
                            << "mode " << static_cast<int>(mode) << " cutoff " << cutoff << " res "
                            << res << " drive " << drive << " sample " << i << ": " << a << " vs "
                            << b;
                    }
                }
            }
        }
    }
}

TEST(HuovilainenLadderTest, ResetClearsStateButKeepsTuningAndMode) {
    Ported f;
    f.Init(kSr);
    f.SetFilterMode(Ported::Mode::HP12);
    f.SetInputDrive(1.0f);
    f.SetFreq(600.0f);
    f.SetRes(0.4f);
    Ported fresh = f;  // same tuning, untouched state
    for (int i = 0; i < 2000; ++i)
        f.Process(1.0f);
    f.Reset();
    EXPECT_EQ(f.GetFilterMode(), Ported::Mode::HP12);
    for (int i = 0; i < 500; ++i) {
        const float in = 0.3f * std::sin(0.05f * static_cast<float>(i));
        ASSERT_TRUE(SameBits(f.Process(in), fresh.Process(in))) << "sample " << i;
    }
}

TEST(HuovilainenLadderTest, TwoTimesVariantIsTheSameFilterToWithinOversamplingError) {
    // Same model at half the oversampling: the linear response must match
    // the 4x variant closely across the band (the interpolation and the
    // unit-delay feedback are what differ), and it must still be a resonant
    // lowpass that survives full drive and resonance.
    for (float cutoff: {200.0f, 1000.0f, 4000.0f}) {
        Ported hq;
        Lite lite;
        hq.Init(kSr);
        lite.Init(kSr);
        for (auto mode: {Ported::Mode::LP24, Ported::Mode::LP12}) {
            hq.SetFilterMode(mode);
            lite.SetFilterMode(mode == Ported::Mode::LP24 ? Lite::Mode::LP24 : Lite::Mode::LP12);
            hq.SetInputDrive(1.0f);
            lite.SetInputDrive(1.0f);
            hq.SetFreq(cutoff);
            lite.SetFreq(cutoff);
            hq.SetRes(0.3f);
            lite.SetRes(0.3f);
            hq.Reset();
            lite.Reset();
            for (float probe: {cutoff * 0.1f, cutoff * 0.5f, cutoff, cutoff * 2.0f}) {
                hq.Reset();
                lite.Reset();
                const float a = SteadyStateGain(hq, probe);
                const float b = SteadyStateGain(lite, probe);
                EXPECT_NEAR(b, a, 0.12f * std::max(a, 0.05f) + 0.01f)
                    << "cutoff " << cutoff << " probe " << probe << " mode "
                    << static_cast<int>(mode);
            }
        }
    }
    Lite flat, peaky;
    flat.Init(kSr);
    peaky.Init(kSr);
    for (Lite* f: {&flat, &peaky}) {
        f->SetInputDrive(1.0f);
        f->SetFreq(1000.0f);
        f->Reset();
    }
    flat.SetRes(0.0f);
    peaky.SetRes(0.8f);
    EXPECT_GT(SteadyStateGain(peaky, 1000.0f), 1.5f * SteadyStateGain(flat, 1000.0f));
    Lite hot;
    hot.Init(kSr);
    hot.SetInputDrive(4.0f);
    hot.SetRes(1.8f);
    for (int i = 0; i < 20000; ++i) {
        if (i % 48 == 0)
            hot.SetFreq(20.0f + static_cast<float>(i % 240) * 90.0f);
        const float out = hot.Process(std::sin(static_cast<float>(i) * 0.1f));
        ASSERT_TRUE(std::isfinite(out));
        ASSERT_LT(std::fabs(out), 100.0f);
    }
}
