// Host tests for the Stage A paraphonic envelope law
// (paraphonic_envelope.hpp): retrigger on every note-on, release only when
// the last held voice releases. Times are in seconds at the 1 kHz tick
// rate, so N ms == N ticks.

#include "paraphonic_envelope.hpp"

#include <gtest/gtest.h>

using WaveX::AudioEngine::ParaphonicEnvelope;

namespace {

class ParaphonicEnvelopeTest : public ::testing::Test {
   protected:
    void SetUp() override {
        env_.Init(1000);
        // 10 ms attack, 20 ms decay, sustain 0.5, 50 ms release.
        env_.SetParams(0.010f, 0.020f, 0.5f, 0.050f);
    }

    // Runs `n` ticks with a steady gate, returns the last level.
    float Run(int n, bool held) {
        float level = 0.0f;
        for (int i = 0; i < n; ++i) {
            level = env_.Tick(false, held);
        }
        return level;
    }

    // Runs `n` ticks, returns the PEAK level seen (the attack apex decays
    // into sustain immediately, so sampling the last level misses it).
    float RunPeak(int n, bool held) {
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) {
            float level = env_.Tick(false, held);
            if (level > peak)
                peak = level;
        }
        return peak;
    }

    ParaphonicEnvelope env_;
};

TEST_F(ParaphonicEnvelopeTest, NoteOnRampsToSustainWhileHeld) {
    env_.Tick(true, true);              // note-on edge
    float peak = RunPeak(15, true);     // through the attack
    EXPECT_GT(peak, 0.9f);              // reached (near) full scale
    float sustain = Run(40, true);      // through the decay
    EXPECT_NEAR(sustain, 0.5f, 0.01f);  // parked at sustain while held
    EXPECT_FALSE(env_.IsReleasing());
}

TEST_F(ParaphonicEnvelopeTest, ReleasesOnlyWhenNoVoiceHeld) {
    env_.Tick(true, true);
    Run(60, true);  // settle at sustain

    // Voices still held: stays at sustain.
    Run(20, true);
    EXPECT_FALSE(env_.IsReleasing());
    EXPECT_NEAR(env_.Level(), 0.5f, 0.01f);

    // Last voice released: envelope enters release and decays to idle.
    Run(1, false);
    EXPECT_TRUE(env_.IsReleasing());
    Run(80, false);
    EXPECT_TRUE(env_.IsIdle());
    EXPECT_FLOAT_EQ(env_.Level(), 0.0f);
}

TEST_F(ParaphonicEnvelopeTest, RetriggersFromCurrentLevelOnEveryNoteOn) {
    env_.Tick(true, true);
    Run(60, true);  // at sustain (0.5)

    // Second note while the first is held: retrigger ramps UP from the
    // current level (no reset to zero - that would click the shared VCA).
    float before = env_.Level();
    env_.Tick(true, true);
    float after = Run(3, true);
    EXPECT_GE(after, before);

    // And it climbs back toward full scale.
    float peak = RunPeak(12, true);
    EXPECT_GT(peak, 0.9f);
}

TEST_F(ParaphonicEnvelopeTest, RetriggerDuringReleaseReopensGate) {
    env_.Tick(true, true);
    Run(60, true);
    Run(20, false);  // releasing
    ASSERT_TRUE(env_.IsReleasing());
    float mid_release = env_.Level();
    EXPECT_GT(mid_release, 0.0f);

    // New note mid-release: retrigger from wherever the level is.
    env_.Tick(true, true);
    EXPECT_FALSE(env_.IsReleasing());
    float peak = RunPeak(15, true);
    EXPECT_GT(peak, 0.9f);
}

TEST_F(ParaphonicEnvelopeTest, IdleStaysSilentWithoutNotes) {
    EXPECT_FLOAT_EQ(Run(50, false), 0.0f);
    EXPECT_TRUE(env_.IsIdle());
}

}  // namespace
