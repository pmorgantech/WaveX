
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
