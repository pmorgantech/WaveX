
#include "audio/voice_lfo.hpp"

#include <gtest/gtest.h>

#include <cmath>
using namespace WaveX::AudioEngine;
using WaveX::Protocol::InstLfoSettings;
TEST(VoiceLfo, RetriggerAndFreeRunUseDistinctClockOwnership) {
    InstLfoSettings s;
    s.wave = 2;
    VoiceLfo gated, free;
    auto beat = VoiceLfo::BeatStep(120, 1000);
    gated.Start(s, 1000, 1, 250, 0, beat, 0, 19, 0);
    EXPECT_NEAR(gated.Value(), -1, 1e-6);
    s.retrigger = 0;
    free.Start(s, 1000, 1, 250, 0, beat, 0, 19, 0);
    EXPECT_NEAR(free.Phase(), .25, 1e-5);
    EXPECT_NEAR(free.Value(), -.5, 1e-5);
    VoiceLfo later;
    later.Start(s, 1000, 1, 375, 0, beat, 0, 99, 0);
    free.Advance(125, beat);
    EXPECT_NEAR(free.Value(), later.Value(), 1e-5);
}
TEST(VoiceLfo, DelayFadeAdvancesOnlyActiveFrames) {
    InstLfoSettings s;
    s.wave = 3;
    s.delay_s = .1f;
    s.fade_s = .2f;
    VoiceLfo l;
    auto beat = VoiceLfo::BeatStep(120, 1000);
    l.Start(s, 1000, 1, 0, 0, beat, 47, 19, 0);
    EXPECT_EQ(l.Advance(0, beat), 0);
    EXPECT_EQ(l.Advance(100, beat), 0);
    EXPECT_NEAR(l.Advance(100, beat), .5, 1e-5);
    EXPECT_NEAR(l.Advance(100, beat), 1, 1e-5);
}
TEST(VoiceLfo, TempoDivisionsFollowTempoAndPitchFollowOnlyAffectsHz) {
    InstLfoSettings s;
    s.wave = 2;
    s.sync_div = 3;
    s.pitch_follow = 1;
    VoiceLfo l;
    auto beat = VoiceLfo::BeatStep(120, 1000);
    l.Start(s, 1000, 2, 0, 0, beat, 0, 19, 0);
    l.Advance(125, beat);
    EXPECT_NEAR(l.Phase(), .25, 1e-5);
    l.Advance(125, VoiceLfo::BeatStep(60, 1000));
    EXPECT_NEAR(l.Phase(), .375, 1e-5);
    s.sync_div = 0;
    l.Start(s, 1000, 2, 0, 0, beat, 0, 19, 0);
    l.Advance(125, beat);
    EXPECT_NEAR(l.Phase(), .25, 1e-5);
    s.sync_div = 7;
    l.Start(s, 1000, 2, 0, 0, beat, 0, 19, 0);
    l.Advance(1000, beat);
    EXPECT_NEAR(l.Phase(), .125, 1e-5);
}
TEST(VoiceLfo, SampleHoldIsStableWithinCycleAndFreeRunSurvivesFrameClockWrap) {
    InstLfoSettings s;
    s.wave = 4;
    s.retrigger = 0;
    VoiceLfo a, b;
    auto beat = VoiceLfo::BeatStep(120, 1000);
    const uint64_t clock = uint64_t{1} << 32;
    a.Start(s, 1000, 1, clock - 10, 0, beat, 0, 19, 0);
    a.Advance(20, beat);
    b.Start(s, 1000, 1, clock + 10, 0, beat, 0, 99, 0);
    EXPECT_FLOAT_EQ(a.Value(), b.Value());
    auto old = b.Value();
    b.Advance(1, beat);
    EXPECT_FLOAT_EQ(old, b.Value());
    b.Advance(1000, beat);
    EXPECT_NE(old, b.Value());
}
TEST(VoiceLfo, SineReferenceAndUnsupportedSettings) {
    InstLfoSettings s;
    VoiceLfo l;
    auto beat = VoiceLfo::BeatStep(120, 1000);
    l.Start(s, 1000, 1, 0, 0, beat, 0, 19, 0);
    EXPECT_NEAR(l.Advance(250, beat), 1, 1e-5);
    EXPECT_NEAR(l.Advance(500, beat), -1, 1e-5);
    s.wave = 200;
    l.Start(s, 1000, 1, 0, 0, beat, 0, 19, 0);
    EXPECT_EQ(l.Advance(250, beat), 0);
}

TEST(VoiceLfo, CmsisSineMatchesTheReferenceWithinInterpolationError) {
    float maximum = 0;
    for (int i = 0; i <= 16384; ++i) {
        const float radians = static_cast<float>(i) * 6.2831853071795865f / 16384;
        maximum = std::max(maximum, std::fabs(LfoSine(radians) - std::sin(radians)));
    }
    EXPECT_LT(maximum, .00003f);
}

TEST(VoiceLfo, EditingDelayFadeUsesActualNoteAgeAndNeverRestartsPhase) {
    InstLfoSettings s;
    s.wave = 3;
    s.rate_hz = .02f;
    VoiceLfo l;
    const auto beat = VoiceLfo::BeatStep(120, 1000);
    l.Start(s, 1000, 1, 0, 0, beat, 0, 19, 0);
    l.Advance(5000, beat);
    const float phase = l.Phase();
    s.delay_s = 4;
    s.fade_s = 2;
    l.UpdateSettings(s, 1000, 1);
    EXPECT_FLOAT_EQ(l.Phase(), phase);
    EXPECT_NEAR(l.Value(), .5f, 1e-6);
    s.delay_s = 6;
    l.UpdateSettings(s, 1000, 1);
    EXPECT_FLOAT_EQ(l.Value(), 0);
    l.Advance(2000, beat);
    EXPECT_NEAR(l.Value(), .5f, 1e-6);
    const auto next = l.Phase();
    s.sync_div = 3;
    s.retrigger = 0;
    l.UpdateSettings(s, 1000, 1);
    EXPECT_FLOAT_EQ(l.Phase(), next);
    l.Advance(125, beat);
    EXPECT_NEAR(l.Phase(), next + .25f, 1e-5);
}

TEST(VoiceLfo, ExtendedHzRangeAndPitchFollowClampReachAudibleRuntime) {
    for (float rate: {.01f, .1f, 100.f}) {
        InstLfoSettings s;
        s.rate_hz = rate;
        VoiceLfo l;
        l.Start(s, 48000, 1, 0, 0, 0, 0, 1, 0);
        const auto frames = static_cast<uint32_t>(12000.0 / rate);
        l.Advance(frames, 0);
        EXPECT_NEAR(l.Phase(), .25f, .0003f) << rate;
    }
    InstLfoSettings s;
    s.rate_hz = 100;
    s.pitch_follow = 1;
    VoiceLfo l;
    l.Start(s, 48000, 4, 0, 0, 0, 0, 1, 0);
    l.Advance(120, 0);
    EXPECT_NEAR(l.Phase(), .25f, 1e-5);
}
TEST(VoiceLfo, ExistingDivisionIdentitiesAndThreeSixteenthsHaveCorrectPeriods) {
    const double beats_per_cycle[] = {0, .25, .5, 1, 2, 4, 8, 16, .75};
    for (uint8_t id = 1; id <= 8; ++id) {
        InstLfoSettings s;
        s.sync_div = id;
        s.pitch_follow = 1;
        VoiceLfo l;
        const auto beat = VoiceLfo::BeatStep(120, 48000);
        l.Start(s, 48000, 4, 0, 0, beat, 0, 1, 0);
        l.Advance(static_cast<uint32_t>(6000 * beats_per_cycle[id]), beat);
        EXPECT_NEAR(l.Phase(), .25f, .00002f) << unsigned(id);
    }
}
TEST(VoiceLfo, SyncModeEditPreservesHeldPhaseAndUsesCurrentTempo) {
    InstLfoSettings s;
    s.rate_hz = 100;
    VoiceLfo l;
    l.Start(s, 48000, 1, 0, 0, 0, 0, 1, 0);
    l.Advance(120, 0);
    const auto phase = l.Phase();
    s.sync_div = 8;
    l.UpdateSettings(s, 48000, 1);
    EXPECT_FLOAT_EQ(l.Phase(), phase);
    l.Advance(9000, VoiceLfo::BeatStep(120, 48000));
    EXPECT_NEAR(l.Phase(), .75f, .00002f);
    l.Advance(9000, VoiceLfo::BeatStep(60, 48000));
    EXPECT_NEAR(std::min(l.Phase(), 1.f - l.Phase()), 0, .00002f);
}

TEST(VoiceLfo, ReusedConfigurationStillRestartsDelayPhaseAndRandomSeed) {
    InstLfoSettings s;
    s.wave = 4;
    s.delay_s = .01f;
    s.fade_s = .02f;
    VoiceLfo reused;
    for (uint32_t seed: {17u, 29u, 101u}) {
        VoiceLfo fresh;
        reused.Start(s, 1000, 1, 375, 0, 0, 0, seed, 0);
        fresh.Start(s, 1000, 1, 375, 0, 0, 0, seed, 0);
        EXPECT_FLOAT_EQ(reused.Phase(), 0);
        EXPECT_FLOAT_EQ(reused.Value(), 0);
        for (uint32_t frames: {5u, 10u, 20u, 1000u}) {
            EXPECT_FLOAT_EQ(reused.Advance(frames, 0), fresh.Advance(frames, 0));
            EXPECT_FLOAT_EQ(reused.Phase(), fresh.Phase());
        }
    }
    s.retrigger = 0;
    for (uint64_t clock: {375u, 750u}) {
        VoiceLfo fresh;
        reused.Start(s, 1000, 1, clock, 0, 0, 3, 1, 0);
        fresh.Start(s, 1000, 1, clock, 0, 0, 3, 1, 0);
        EXPECT_FLOAT_EQ(reused.Phase(), fresh.Phase());
        EXPECT_FLOAT_EQ(reused.Advance(30, 0), fresh.Advance(30, 0));
    }
}

TEST(VoiceLfo, UnchangedSettingsReconfigureForSampleRateAndFollowedPitch) {
    InstLfoSettings s;
    s.wave = 2;
    s.pitch_follow = 1;
    VoiceLfo l;
    l.Start(s, 1000, 1, 0, 0, 0, 0, 1, 0);
    l.Advance(125, 0);
    EXPECT_NEAR(l.Phase(), .125f, 1e-5);
    l.UpdateSettings(s, 1000, 2);
    EXPECT_NEAR(l.Phase(), .125f, 1e-5);
    l.Advance(125, 0);
    EXPECT_NEAR(l.Phase(), .375f, 1e-5);
    l.UpdateSettings(s, 2000, 2);
    l.Advance(125, 0);
    EXPECT_NEAR(l.Phase(), .5f, 1e-5);
    l.Start(s, 2000, 4, 0, 0, 0, 0, 1, 0);
    l.Advance(125, 0);
    EXPECT_NEAR(l.Phase(), .25f, 1e-5);
    l.Start(s, 0, 1, 0, 0, 0, 0, 1, 0);
    l.Advance(12000, 0);
    EXPECT_NEAR(l.Phase(), .25f, 1e-5);
}
