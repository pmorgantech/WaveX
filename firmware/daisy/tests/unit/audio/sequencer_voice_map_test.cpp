#include "audio/sequencer_voice_map.hpp"

#include <gtest/gtest.h>

#include "audio/snapshot_mailbox.hpp"

using namespace WaveX::AudioEngine;

namespace {

class SequencerVoiceMapTest : public ::testing::Test {
   protected:
    int16_t samples[16][64] = {};
    SequencerVoiceMap map;

    void Build(uint16_t loaded, uint16_t unavailable = 0) {
        map.Rebuild(
            [&](uint8_t track, uint8_t note, uint8_t velocity, VoiceTriggerParams* out, uint8_t max)
                -> uint8_t {
                EXPECT_EQ(note, 60);
                EXPECT_EQ(velocity, 127);
                EXPECT_EQ(max, kMaxLayerTriggers);
                if ((loaded & (1u << track)) == 0)
                    return 0;
                auto& p = out[0];
                p = VoiceTriggerParams{};
                p.sample = samples[track];
                p.sample_frames = 64;
                p.track = track;
                p.note = note;
                p.trigger_note = note;
                p.root_note = note;
                p.velocity = velocity;
                p.loop = true;
                p.loop_end = 64;
                p.filter_cutoff_hz = 48000.0f;
                p.attack_s = 0;
                p.decay_s = 0;
                p.sustain_level = 1;
                p.release_s = 0;
                return 1;
            },
            unavailable);
    }
};

TEST_F(SequencerVoiceMapTest, EveryRowAddressesItsOwnTrackIncludingTrackSixteen) {
    Build(0xFFFFu);
    for (uint8_t track = 0; track < 16; ++track) {
        ASSERT_EQ(map.layer_count[track], 1);
        EXPECT_EQ(map.layers[track][0].track, track);
        EXPECT_EQ(map.layers[track][0].sample, samples[track]);
    }
}

TEST_F(SequencerVoiceMapTest, EmptyOrLoadingTracksNeverBorrowTrackOne) {
    Build(0x8005u, 1u << 2);
    EXPECT_EQ(map.layer_count[0], 1);
    EXPECT_EQ(map.layer_count[2], 0);
    EXPECT_EQ(map.layer_count[15], 1);
    EXPECT_EQ(map.layer_count[1], 0);
    bool resolved_loading_track = false;
    map.Rebuild(
        [&](uint8_t track, uint8_t, uint8_t, VoiceTriggerParams*, uint8_t) {
            resolved_loading_track |= track == 2;
            return uint8_t{0};
        },
        1u << 2);
    EXPECT_FALSE(resolved_loading_track);
}

TEST_F(SequencerVoiceMapTest, RevocationPublishesBeforeRetirementAndPreservesOtherTracks) {
    Build(0x8005u);
    SnapshotMailbox<SequencerVoiceMap> mailbox;
    mailbox.Init(map);
    SequencerVoiceMap active = map;

    map.Revoke(1u << 15);
    mailbox.Publish(map);
    ASSERT_TRUE(mailbox.ConsumeLatest(active));
    EXPECT_EQ(active.layer_count[15], 0);
    EXPECT_EQ(active.layer_count[0], 1);
    EXPECT_EQ(active.layer_count[2], 1);
    EXPECT_EQ(active.layers[2][0].sample, samples[2]);

    // A completed replacement publishes new prepared data for the same Track.
    Build(0x8005u);
    map.layers[15][0].sample = samples[14];
    mailbox.Publish(map);
    ASSERT_TRUE(mailbox.ConsumeLatest(active));
    EXPECT_EQ(active.layer_count[15], 1);
    EXPECT_EQ(active.layers[15][0].sample, samples[14]);
    EXPECT_EQ(active.layers[0][0].sample, samples[0]);

    map.Revoke(0xFFFFu);
    for (auto count: map.layer_count)
        EXPECT_EQ(count, 0);
}

TEST_F(SequencerVoiceMapTest, LayerCountsCannotExceedCallbackArrayBounds) {
    map.Rebuild(
        [](uint8_t, uint8_t, uint8_t, VoiceTriggerParams*, uint8_t) { return uint8_t{255}; }, 0);
    for (auto count: map.layer_count)
        EXPECT_EQ(count, kMaxLayerTriggers);
}

TEST_F(SequencerVoiceMapTest, FourPreparedTracksCanBeReleasedIndependently) {
    Build(0x8007u);
    VoiceManager voices;
    voices.Init(48000);
    const uint8_t tracks[] = {0, 1, 2, 15};
    for (uint8_t track: tracks)
        voices.Trigger(map.layers[track][0]);
    ASSERT_EQ(voices.ActiveVoiceCount(), 4);
    voices.ReleaseTrack(60, 15);
    float left[48] = {}, right[48] = {};
    voices.Render(left, right, 48);
    EXPECT_EQ(voices.ActiveVoiceCount(), 3);
    for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
        const auto& voice = voices.GetVoice(i);
        if (voice.state != VoiceState::Idle) {
            EXPECT_NE(voice.track, 15);
        }
    }
}

}  // namespace
