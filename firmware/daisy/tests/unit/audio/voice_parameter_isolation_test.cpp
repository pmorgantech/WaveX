#include <gtest/gtest.h>

#include "audio/track_live_updates.hpp"
#include "audio/voice_manager.hpp"
#include <array>
#include <cmath>

namespace {
using namespace WaveX::AudioEngine;

class VoiceParameterIsolationTest : public ::testing::Test {
   protected:
    void SetUp() override {
        voices_.Init(48000);
        for (size_t i = 0; i < sample_.size(); ++i) {
            sample_[i] = (i / 3) % 2 ? 16000 : -16000;
        }
    }
    void Trigger(uint8_t track) {
        VoiceTriggerParams params;
        params.sample = sample_.data();
        params.sample_frames = static_cast<uint32_t>(sample_.size());
        params.track = track;
        params.note = static_cast<uint8_t>(60 + track);
        params.root_note = params.note;
        params.attack_s = 0;
        params.decay_s = 0;
        params.sustain_level = 1;
        voices_.Trigger(params);
    }
    void Pitch(uint8_t track, float semitones) {
        VoiceLiveParams params;
        params.track = track;
        params.pitch_semitones = semitones;
        voices_.ApplyLiveParams(params);
    }
    float RenderPeak() {
        voices_.Render(left_.data(), right_.data(), left_.size());
        float peak = 0;
        for (size_t i = 48; i < left_.size(); ++i) {
            peak = std::max(peak, std::fabs(left_[i]));
        }
        return peak;
    }
    static const ModSlot* Resolve(const void* context, uint8_t) {
        return static_cast<const ModSlot*>(context);
    }
    std::array<int16_t, 4096> sample_{};
    std::array<float, 64> left_{}, right_{};
    VoiceManager voices_;
};

TEST_F(VoiceParameterIsolationTest, NewNotesUseTheirOwnTracksPitch) {
    Pitch(0, 12);
    Trigger(1);
    EXPECT_FLOAT_EQ(voices_.GetVoice(0).increment, 1.0f);
    Trigger(0);
    EXPECT_FLOAT_EQ(voices_.GetVoice(1).increment, 2.0f);
    Pitch(1, -12);
    Trigger(0);
    EXPECT_FLOAT_EQ(voices_.GetVoice(2).increment, 2.0f);
}

TEST_F(VoiceParameterIsolationTest, PendingEditsForDifferentTracksBothReachSoundingVoices) {
    Trigger(0);
    Trigger(1);
    TrackLiveUpdates updates;
    updates.Init();
    VoiceLiveParams params;
    params.track = 0;
    params.pitch_semitones = 7;
    updates.Publish(params);
    params.pitch_semitones = 12;
    updates.Publish(params);
    params.track = 1;
    params.pitch_semitones = -12;
    updates.Publish(params);
    updates.ApplyTo(voices_);
    EXPECT_FLOAT_EQ(voices_.GetVoice(0).increment, 2.0f);
    EXPECT_FLOAT_EQ(voices_.GetVoice(1).increment, 0.5f);
    // An unchanged block must not reapply old values over callback state.
    Pitch(0, 0);
    updates.ApplyTo(voices_);
    EXPECT_FLOAT_EQ(voices_.GetVoice(0).increment, 1.0f);
}

TEST_F(VoiceParameterIsolationTest, ReturningPitchModulationToZeroRestoresTheBaseRate) {
    Trigger(0);
    ModSlot slots[kMaxModSlots]{};
    slots[0].source = SRC_MACRO_1;
    slots[0].dest = DEST_PITCH;
    slots[0].depth = 32767;
    const ModSlotResolver resolver{slots, &Resolve};
    ModSources global;
    global.macro[0] = 1;
    voices_.TickModulation(resolver, global, 64);
    RenderPeak();
    ASSERT_GT(voices_.GetVoice(0).increment, 1.0f);
    global.macro[0] = 0;
    voices_.TickModulation(resolver, global, 64);
    RenderPeak();
    EXPECT_FLOAT_EQ(voices_.GetVoice(0).increment, 1.0f);
}

TEST_F(VoiceParameterIsolationTest, RemovingCutoffModulationReopensTheFilter) {
    Trigger(0);
    const float open = RenderPeak();
    ModSlot slots[kMaxModSlots]{};
    slots[0].source = SRC_MACRO_1;
    slots[0].dest = DEST_CUTOFF;
    slots[0].depth = -32767;
    const ModSlotResolver resolver{slots, &Resolve};
    ModSources global;
    global.macro[0] = 1;
    voices_.TickModulation(resolver, global, 64);
    RenderPeak();
    const float closed = RenderPeak();
    ASSERT_LT(closed, open * 0.2f);
    slots[0] = ModSlot{};
    voices_.TickModulation(resolver, global, 64);
    RenderPeak();
    EXPECT_GT(RenderPeak(), closed * 4);
}
}  // namespace
