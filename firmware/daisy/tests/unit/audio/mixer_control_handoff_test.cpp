#include "audio/mixer_control_handoff.hpp"

#include <gtest/gtest.h>

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
}  // namespace
