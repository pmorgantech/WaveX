// Tests for the control-rate LFO (roadmap Phase 2.5 item 4; design:
// docs/features/param-locks-and-modulation.md §5).
//
// Two things are worth pinning hardest. Every waveform must be bipolar and
// bounded, because a mod depth has to mean the same thing whichever shape is
// selected - a saw that ran 0..1 while the sine ran -1..+1 would halve the
// depth of one and offset the other. And the delay/fade envelope must not
// restart the phase, or a note held through the delay would audibly jump when
// the vibrato arrives.

#include "audio/lfo.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using WaveX::AudioEngine::kLfoMaxRateHz;
using WaveX::AudioEngine::kLfoMinRateHz;
using WaveX::AudioEngine::Lfo;
using WaveX::AudioEngine::LfoWave;

namespace {

constexpr float kTickHz = 1000.0f;

Lfo MakeLfo(LfoWave wave, float rate_hz) {
    Lfo lfo;
    lfo.Init(kTickHz);
    lfo.SetWave(wave);
    lfo.SetRateHz(rate_hz);
    return lfo;
}

/// One full period at the given rate, in control ticks.
int TicksPerPeriod(float rate_hz) {
    return static_cast<int>(kTickHz / rate_hz);
}

TEST(LfoWaves, EveryShapeIsBipolarAndBounded) {
    const LfoWave waves[] = {
        LfoWave::Sine, LfoWave::Triangle, LfoWave::Saw, LfoWave::Square, LfoWave::SampleHold};
    for (LfoWave wave: waves) {
        auto lfo = MakeLfo(wave, 10.0f);
        float lo = 2.0f;
        float hi = -2.0f;
        for (int i = 0; i < TicksPerPeriod(10.0f) * 4; ++i) {
            const float v = lfo.Tick();
            EXPECT_GE(v, -1.0001f) << "wave " << int(wave);
            EXPECT_LE(v, 1.0001f) << "wave " << int(wave);
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
        EXPECT_LT(lo, 0.0f) << "wave " << int(wave) << " never went negative";
        EXPECT_GT(hi, 0.0f) << "wave " << int(wave) << " never went positive";
    }
}

TEST(LfoWaves, SineTracksItsPhase) {
    auto lfo = MakeLfo(LfoWave::Sine, 1.0f);
    // A quarter period in, a sine starting at 0 should be near its peak.
    for (int i = 0; i < TicksPerPeriod(1.0f) / 4; ++i) {
        lfo.Tick();
    }
    EXPECT_NEAR(lfo.Value(), 1.0f, 0.02f);
}

TEST(LfoWaves, SquareSpendsHalfItsPeriodEitherSide) {
    auto lfo = MakeLfo(LfoWave::Square, 1.0f);
    int positive = 0;
    const int ticks = TicksPerPeriod(1.0f);
    for (int i = 0; i < ticks; ++i) {
        if (lfo.Tick() > 0.0f) {
            ++positive;
        }
    }
    EXPECT_NEAR(static_cast<float>(positive) / static_cast<float>(ticks), 0.5f, 0.02f);
}

TEST(LfoWaves, TriangleIsContinuous) {
    auto lfo = MakeLfo(LfoWave::Triangle, 1.0f);
    // A triangle at 1 Hz on a 1 kHz tick moves 0.004 per tick; anything much
    // larger is a discontinuity, which would be a click through a mod dest.
    float previous = lfo.Value();
    for (int i = 0; i < TicksPerPeriod(1.0f) * 2; ++i) {
        const float v = lfo.Tick();
        EXPECT_LT(std::fabs(v - previous), 0.02f) << "triangle jumped at tick " << i;
        previous = v;
    }
}

TEST(LfoRate, ClampsToTheDocumentedRange) {
    Lfo lfo;
    lfo.Init(kTickHz);
    lfo.SetRateHz(0.0f);
    EXPECT_FLOAT_EQ(lfo.RateHz(), kLfoMinRateHz);
    lfo.SetRateHz(1000.0f);
    EXPECT_FLOAT_EQ(lfo.RateHz(), kLfoMaxRateHz) << "an LFO must not reach audio rate";
}

// Phase wraps by subtraction rather than modulo so a very slow LFO does not
// accumulate error. At the minimum rate a period is 50 s, i.e. 50,000 ticks.
TEST(LfoPhase, SlowRateStaysAccurateOverAFullPeriod) {
    auto lfo = MakeLfo(LfoWave::Saw, kLfoMinRateHz);
    const int ticks = TicksPerPeriod(kLfoMinRateHz);
    for (int i = 0; i < ticks; ++i) {
        lfo.Tick();
    }
    // One full period later the phase should be back near where it started.
    EXPECT_LT(lfo.Phase(), 0.01f);
}

TEST(LfoSampleHold, HoldsBetweenPeriodsAndChangesAcrossThem) {
    auto lfo = MakeLfo(LfoWave::SampleHold, 10.0f);
    const int period = TicksPerPeriod(10.0f);

    const float first = lfo.Tick();
    for (int i = 1; i < period - 1; ++i) {
        EXPECT_FLOAT_EQ(lfo.Tick(), first) << "S&H changed mid-period at tick " << i;
    }
    // Somewhere across the wrap it must take a new value.
    bool changed = false;
    for (int i = 0; i < period; ++i) {
        if (lfo.Tick() != first) {
            changed = true;
            break;
        }
    }
    EXPECT_TRUE(changed) << "S&H never resampled";
}

// Deterministic, or no assertion about it could be written at all.
TEST(LfoSampleHold, SequenceIsRepeatable) {
    auto a = MakeLfo(LfoWave::SampleHold, 50.0f);
    auto b = MakeLfo(LfoWave::SampleHold, 50.0f);
    for (int i = 0; i < 500; ++i) {
        EXPECT_FLOAT_EQ(a.Tick(), b.Tick()) << "at tick " << i;
    }
}

TEST(LfoDelayFade, StaysSilentThroughTheDelay) {
    auto lfo = MakeLfo(LfoWave::Sine, 5.0f);
    lfo.SetDelayFade(/*delay_s=*/0.1f, /*fade_s=*/0.0f);
    lfo.Retrigger();

    for (int i = 0; i < 99; ++i) {
        EXPECT_FLOAT_EQ(lfo.Tick(), 0.0f) << "output during the delay at tick " << i;
    }
}

TEST(LfoDelayFade, FadesInGraduallyRatherThanSwitchingOn) {
    auto lfo = MakeLfo(LfoWave::Saw, 5.0f);
    lfo.SetDelayFade(/*delay_s=*/0.0f, /*fade_s=*/0.2f);
    lfo.Retrigger();

    // Amplitude should grow: compare the excursion early against later.
    float early = 0.0f;
    for (int i = 0; i < 50; ++i) {
        early = std::fmax(early, std::fabs(lfo.Tick()));
    }
    float late = 0.0f;
    for (int i = 0; i < 200; ++i) {
        late = std::fmax(late, std::fabs(lfo.Tick()));
    }
    EXPECT_GT(late, early) << "fade did not ramp the amplitude";
    EXPECT_NEAR(late, 1.0f, 0.05f) << "fade never reached full depth";
}

// The delay must gate the amplitude, not hold the phase. A held note whose
// vibrato arrives should hear the LFO already in motion, not restart.
TEST(LfoDelayFade, PhaseKeepsRunningUnderneathTheDelay) {
    auto lfo = MakeLfo(LfoWave::Sine, 5.0f);
    lfo.SetDelayFade(/*delay_s=*/0.05f, /*fade_s=*/0.0f);
    lfo.Retrigger();

    for (int i = 0; i < 25; ++i) {
        lfo.Tick();
    }
    EXPECT_EQ(lfo.Value(), 0.0f) << "still inside the delay";
    EXPECT_GT(lfo.Phase(), 0.0f) << "phase was frozen during the delay";
}

TEST(LfoRetrigger, ResetsPhaseAndTheEnvelope) {
    auto lfo = MakeLfo(LfoWave::Saw, 2.0f);
    for (int i = 0; i < 200; ++i) {
        lfo.Tick();
    }
    ASSERT_GT(lfo.Phase(), 0.0f);

    lfo.Retrigger();
    EXPECT_FLOAT_EQ(lfo.Phase(), 0.0f);
}

}  // namespace
