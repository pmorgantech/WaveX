#include "audio/live_note_runtime.hpp"

#include <gtest/gtest.h>

using namespace WaveX::AudioEngine;
namespace {
class LiveNotes : public ::testing::Test {
   protected:
    LiveNoteRuntime notes;
    VoiceManager voices;
    SequencerVoiceMap map;
    int16_t pcm[256]{};
    void SetUp() override {
        notes.Init();
        voices.Init(48000);
        Prepare(1);
    }
    void Prepare(uint8_t layers, bool one_shot = false) {
        Instrument instrument;
        instrument.origin = InstrumentOrigin::Built;
        for (uint8_t i = 0; i < layers; ++i) {
            auto& zone = instrument.osc[0].zones[i];
            zone.in_use = true;
            zone.sample_id = 1;
            zone.key_lo = 0;
            zone.key_hi = 127;
        }
        SampleResolver resolver{this, [](const void* ctx, uint16_t) {
                                    SampleRef ref;
                                    ref.data = static_cast<const LiveNotes*>(ctx)->pcm;
                                    ref.frames = 256;
                                    return ref;
                                }};
        for (uint8_t track = 0; track < 16; ++track) {
            map.PrepareTrack(track, instrument, resolver);
            for (auto& zone: map.tracks[track].zones)
                zone.one_shot = one_shot;
        }
    }
    unsigned Held(uint8_t source, uint32_t serial = 0) const {
        unsigned count = 0;
        for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
            const auto& voice = voices.GetVoice(i);
            if (!voice.IsFree() && !voice.envelope.IsReleasing() && voice.live_note.Valid() &&
                voice.live_note.source == source && (!serial || voice.live_note.serial == serial))
                ++count;
        }
        return count;
    }
};
}  // namespace

TEST_F(LiveNotes, FourLayerSixteenTrackFanoutUsesOneQueueEntry) {
    Prepare(4);
    ASSERT_TRUE(notes.Press(0, 60, 100, 0xffff));
    ASSERT_TRUE(notes.Drain(&map, voices));
    EXPECT_EQ(notes.Refused(), 0u);
    EXPECT_EQ(voices.ActiveChannelCount(), 8);
    EXPECT_EQ(Held(0), 8u);
    for (uint8_t i = 0; i < 8; ++i)
        EXPECT_GE(voices.GetVoice(i).track, 14);
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0), 0u);
}

TEST_F(LiveNotes, RepeatedKeysReleaseFifoAndDoNotReleaseSequencerOrOtherSource) {
    notes.Press(0, 60, 100, 1);
    notes.Press(0, 60, 100, 1);
    notes.Press(16, 60, 100, 1);
    notes.Drain(&map, voices);
    VoiceTriggerParams sequence;
    map.Materialize(map.Select(0, 60, 100), 0, sequence);
    const auto group = voices.TriggerGroup(&sequence, 1);
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0, 1), 0u);
    EXPECT_EQ(Held(0, 2), 1u);
    EXPECT_EQ(Held(16), 1u);
    bool sequence_held = false;
    for (uint8_t i = 0; i < 8; ++i)
        if (voices.GetVoice(i).group_id == group)
            sequence_held = !voices.GetVoice(i).envelope.IsReleasing();
    EXPECT_TRUE(sequence_held);
}

TEST_F(LiveNotes, StolenOldPressCannotReleaseItsReplacement) {
    notes.Press(0, 60, 100, 1);
    notes.Drain(&map, voices);
    for (uint8_t pitch = 61; pitch < 69; ++pitch)
        notes.Press(1, pitch, 100, 2);
    notes.Drain(&map, voices);
    notes.Press(0, 60, 100, 1);
    notes.Drain(&map, voices);
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0, 2), 1u);
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0), 0u);
}

TEST_F(LiveNotes, ReleaseKeepsOriginalDestinationsAndSources) {
    notes.Press(0, 60, 100, 3);
    notes.Press(1, 60, 100, 1);
    notes.Drain(&map, voices);
    // MIDI routing may now be Off or point elsewhere: release has no mask.
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0), 0u);
    EXPECT_EQ(Held(1), 1u);
}

TEST_F(LiveNotes, RefusedPressStillConsumesItsOwnRelease) {
    for (uint8_t i = 0; i < 64; ++i)
        ASSERT_TRUE(notes.Press(1, 61, 100, 0));
    EXPECT_FALSE(notes.Press(0, 60, 100, 1));
    notes.Drain(&map, voices);
    notes.Drain(&map, voices);
    ASSERT_TRUE(notes.Press(0, 60, 100, 1));
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0, 2), 1u);
    notes.Release(0, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0), 0u);
}

TEST_F(LiveNotes, OverflowReleaseCoversPendingTriggersAndKeepsLaterPress) {
    for (uint8_t i = 0; i < 64; ++i)
        ASSERT_TRUE(notes.Press(0, 60, 100, 0xffff));
    for (uint8_t i = 0; i < 64; ++i)
        EXPECT_FALSE(notes.Release(0, 60));
    for (uint8_t i = 0; i < 32; ++i) {
        notes.Drain(&map, voices);
        EXPECT_EQ(Held(0), 0u);
    }
    notes.Press(0, 60, 100, 1);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(0, 65), 1u);
}

TEST_F(LiveNotes, StopInvalidatesQueuedBindingsButAllowsLaterPresses) {
    for (uint8_t i = 0; i < 8; ++i)
        notes.Press(0, 60, 100, 0xffff);
    notes.Drain(&map, voices);  // consumes exactly two routed presses
    notes.StopTracks(0xffff, voices);
    for (uint8_t i = 0; i < 3; ++i)
        EXPECT_FALSE(notes.Drain(&map, voices));
    EXPECT_EQ(voices.ActiveVoiceCount(), 0);
    notes.Press(0, 60, 100, 1);
    EXPECT_TRUE(notes.Drain(&map, voices));
    EXPECT_EQ(Held(0), 1u);
}

TEST_F(LiveNotes, UnmatchedReleaseDoesNotConsumeNextPressAndOneShotsIgnoreRelease) {
    Prepare(1, true);
    notes.Release(16, 60);
    notes.Press(16, 60, 100, 1);
    notes.Release(16, 60);
    notes.Drain(&map, voices);
    EXPECT_EQ(Held(16, 1), 1u);
}
