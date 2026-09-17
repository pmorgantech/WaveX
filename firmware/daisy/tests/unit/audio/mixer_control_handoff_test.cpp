#include "audio/mixer_control_handoff.hpp"

#include <gtest/gtest.h>

#include "audio/master_gain.hpp"

namespace {
using namespace WaveX;
using namespace WaveX::Protocol;

class MixerControlHandoffTest : public ::testing::Test {
   protected:
    void SetUp() override {
        handoff_.Init();
        mixer_.Reset();
        mixer_.SetSampleRate(48000);
    }
    void Update(uint8_t op, uint8_t track, uint16_t value) {
        MixOpMessage message{};
        message.op = op;
        message.track = track;
        message.value = value;
        handoff_.Update(message);
    }
    AudioEngine::MixerControlHandoff handoff_;
    Mix::TrackMixer mixer_;
};

TEST_F(MixerControlHandoffTest, MainEditsAreInvisibleUntilCallbackAppliesTheCompleteTable) {
    Update(MIX_OP_SET_MUTE_MASK, 0, 0xfffe);
    Update(MIX_OP_SET_GAIN, 3, Mix::GainDbToWire(-6));
    Update(MIX_OP_SET_PAN, 7, Mix::PanToWire(-1));
    for (uint8_t track = 0; track < Mix::kNumTracks; ++track) {
        EXPECT_FALSE(mixer_.Track(track).mute);
    }
    EXPECT_FLOAT_EQ(mixer_.Track(3).gain, 1);
    EXPECT_FLOAT_EQ(mixer_.PanOffsetFor(7), 0);
    handoff_.ApplyTo(mixer_);
    for (uint8_t track = 0; track < Mix::kNumTracks; ++track) {
        EXPECT_EQ(mixer_.Track(track).mute, track != 0);
    }
    EXPECT_FLOAT_EQ(mixer_.Track(3).gain, Mix::DbToLinear(-6));
    EXPECT_FLOAT_EQ(mixer_.PanOffsetFor(7), -1);
}

TEST_F(MixerControlHandoffTest, LaterControlsPreserveTheCallbacksInProgressMuteRamp) {
    Update(MIX_OP_SET_MUTE, 0, 1);
    handoff_.ApplyTo(mixer_);
    mixer_.Tick(48);
    ASSERT_NEAR(mixer_.GainFor(0), 0.8f, 0.0001f);
    Update(MIX_OP_SET_PAN, 1, Mix::PanToWire(1));
    handoff_.ApplyTo(mixer_);
    EXPECT_NEAR(mixer_.GainFor(0), 0.8f, 0.0001f);
    mixer_.Tick(48);
    EXPECT_NEAR(mixer_.GainFor(0), 0.6f, 0.0001f);
    Update(MIX_OP_SET_MUTE_MASK, 0, 0);
    handoff_.ApplyTo(mixer_);
    mixer_.Tick(48);
    EXPECT_NEAR(mixer_.GainFor(0), 0.8f, 0.0001f);
}

TEST_F(MixerControlHandoffTest, InvalidTrackDoesNotReplacePendingValidControls) {
    Update(MIX_OP_SET_MUTE, 15, 1);
    Update(MIX_OP_SET_GAIN, 16, 0);
    Update(MIX_OP_SET_MUTE, 255, 0);
    handoff_.ApplyTo(mixer_);
    EXPECT_TRUE(mixer_.Track(15).mute);
    EXPECT_FLOAT_EQ(mixer_.Track(0).gain, 1);
}
TEST_F(MixerControlHandoffTest, ReadbackUsesAcceptedTargetsAndPreservesTrackIdentity) {
    auto state = handoff_.Read({17, 15});
    ASSERT_TRUE(IsValidMixState(state));
    EXPECT_EQ(state.gain, 6000);
    EXPECT_EQ(state.pan, 32768);
    EXPECT_EQ(state.request_id, 17);
    EXPECT_EQ(state.track, 15);
    for (uint16_t pan: {0, 32767, 32768, 65535}) {
        Update(MIX_OP_SET_PAN, 15, pan);
        EXPECT_EQ(handoff_.Read({18, 15}).pan, pan);
    }
    Update(MIX_OP_SET_GAIN, 15, 5400);
    Update(MIX_OP_SET_MUTE, 15, 1);
    state = handoff_.Read({19, 15});
    EXPECT_EQ(state.gain, 5400);
    EXPECT_EQ(state.mute, 1);
    EXPECT_EQ(handoff_.Read({20, 0}).gain, 6000);
    // Reading main-loop targets does not prematurely mutate the audio state.
    EXPECT_FLOAT_EQ(mixer_.Track(15).gain, 1);
    handoff_.ApplyTo(mixer_);
    EXPECT_FLOAT_EQ(mixer_.Track(15).gain, Mix::DbToLinear(-6));
    EXPECT_FALSE(handoff_.Read({0, 0}).valid);
    EXPECT_FALSE(handoff_.Read({1, 16}).valid);
}

TEST_F(MixerControlHandoffTest, SoloPreservesUserMutesAndEditsMadeWhileSoloed) {
    Update(MIX_OP_SET_MUTE, 2, 1);
    Update(MIX_OP_SET_SOLO_MASK, 0, 1);
    handoff_.ApplyTo(mixer_);
    EXPECT_FALSE(mixer_.Track(0).mute);
    EXPECT_TRUE(mixer_.Track(1).mute);
    EXPECT_TRUE(mixer_.Track(2).mute);
    EXPECT_FALSE(handoff_.Read({1, 1}).mute);
    EXPECT_TRUE(handoff_.Read({2, 2}).mute);
    Update(MIX_OP_SET_MUTE, 2, 0);
    Update(MIX_OP_SET_MUTE, 3, 1);
    Update(MIX_OP_SET_SOLO_MASK, 0, 0);
    handoff_.ApplyTo(mixer_);
    for (uint8_t track = 0; track < Mix::kNumTracks; ++track)
        EXPECT_EQ(mixer_.Track(track).mute, track == 3);
}
TEST_F(MixerControlHandoffTest, SoloSwitchAndClearAreAtomicAndKeepManualMutePriority) {
    Update(MIX_OP_SET_MUTE, 15, 1);
    Update(MIX_OP_SET_SOLO_MASK, 0, 0x8001);
    handoff_.ApplyTo(mixer_);
    EXPECT_FALSE(mixer_.Track(0).mute);
    EXPECT_TRUE(mixer_.Track(1).mute);
    EXPECT_TRUE(mixer_.Track(15).mute);
    Update(MIX_OP_SET_SOLO_MASK, 0, 2);
    EXPECT_FALSE(mixer_.Track(0).mute);
    handoff_.ApplyTo(mixer_);
    EXPECT_TRUE(mixer_.Track(0).mute);
    EXPECT_FALSE(mixer_.Track(1).mute);
    Update(MIX_OP_SET_SOLO_MASK, 0, 0);
    handoff_.ApplyTo(mixer_);
    EXPECT_FALSE(mixer_.Track(0).mute);
    EXPECT_TRUE(mixer_.Track(15).mute);
}

}  // namespace

TEST(MasterGainTest, RampsBothDirectionsAndRetargetsFromCurrentGain) {
    WaveX::AudioEngine::MasterGain gain;
    gain.Init(48000);
    EXPECT_FLOAT_EQ(gain.Next(), 1);
    gain.SetTarget(0);
    for (int i = 0; i < 120; ++i) {
        gain.SetTarget(0);  // block refresh never restarts an unchanged ramp
        EXPECT_NEAR(gain.Next(), 1.0f - (i + 1) / 240.0f, 0.00001f);
    }
    gain.SetTarget(2);
    for (int i = 0; i < 240; ++i)
        EXPECT_NEAR(gain.Next(), 0.5f + (i + 1) * 1.5f / 240, 0.00002f);
    EXPECT_FLOAT_EQ(gain.Next(), 2);
    gain.SetTarget(0);
    for (int i = 0; i < 240; ++i)
        gain.Next();
    EXPECT_FLOAT_EQ(gain.Next(), 0);
}
TEST(MixerMasterTest, ReadbackAndCallbackShareOneTarget) {
    WaveX::AudioEngine::MixerControlHandoff handoff;
    WaveX::Mix::TrackMixer mixer;
    handoff.Init();
    for (uint16_t value: {0, 5400, 6000, 6600}) {
        handoff.Update({MIX_OP_SET_MASTER, 0, value});
        auto state = handoff.Read({42, MIX_MASTER_TRACK});
        ASSERT_TRUE(IsValidMixState(state));
        EXPECT_EQ(state.gain, value);
        handoff.ApplyTo(mixer);
        EXPECT_FLOAT_EQ(mixer.MasterGain(),
                        WaveX::Mix::DbToLinear(WaveX::Mix::WireToGainDb(value)));
    }
}
