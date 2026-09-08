#include "audio/sequencer_voice_map.hpp"

#include <gtest/gtest.h>

#include "audio/snapshot_mailbox.hpp"

using namespace WaveX::AudioEngine;
namespace {
class SequencerVoiceMapTest : public ::testing::Test {
   protected:
    int16_t samples[32][64]{};
    Instrument instrument;
    SequencerVoiceMap map;
    SampleResolver resolver{this, [](const void* ctx, uint16_t id) {
                                const auto* self = static_cast<const SequencerVoiceMapTest*>(ctx);
                                SampleRef ref;
                                if (id > 0 && id <= 32) {
                                    ref.data = self->samples[id - 1];
                                    ref.frames = 64;
                                    ref.gain_mul = 0.7f;
                                    ref.start_frame = 2;
                                    ref.end_frame = 62;
                                    ref.fade_out_ms = 3;
                                    ref.sample_rate_hz = 44100;
                                }
                                return ref;
                            }};
    void SetUp() override {
        instrument.origin = InstrumentOrigin::Built;
        for (uint8_t i = 0; i < kMaxZones; ++i) {
            auto& z = instrument.zones[i];
            z.in_use = true;
            z.sample_id = i + 1;
            z.key_lo = i * 4;
            z.key_hi = i * 4 + 7;
            z.vel_lo = i % 2 ? 50 : 1;
            z.vel_hi = i % 2 ? 127 : 90;
            z.flags = i % 2 ? ZONE_FLAG_VEL_XFADE_DOWN : ZONE_FLAG_VEL_XFADE;
            z.coarse_tune = static_cast<int8_t>(i % 12);
            z.fine_tune = -23;
            z.gain = 0.3f;
            z.root_note = 48;
        }
    }
};
TEST_F(SequencerVoiceMapTest, PreparedResolutionMatchesLiveNotesAndVelocityLayers) {
    for (auto mode: {InstrumentMode::Keyboard, InstrumentMode::Drum}) {
        instrument.mode = mode;
        map.PrepareTrack(15, instrument, resolver);
        for (int note = 0; note < 128; ++note) {
            for (int velocity = 1; velocity < 128; ++velocity) {
                VoiceTriggerParams expected[4], actual[4];
                const auto count = ResolveNoteOn(instrument,
                                                 15,
                                                 static_cast<uint8_t>(note),
                                                 static_cast<uint8_t>(velocity),
                                                 resolver,
                                                 expected,
                                                 4);
                ASSERT_EQ(
                    map.Resolve(
                        15, static_cast<uint8_t>(note), static_cast<uint8_t>(velocity), actual),
                    count);
                for (uint8_t i = 0; i < count; ++i) {
                    EXPECT_EQ(actual[i].sample, expected[i].sample);
                    EXPECT_EQ(actual[i].note, expected[i].note);
                    EXPECT_EQ(actual[i].trigger_note, note);
                    EXPECT_EQ(actual[i].velocity, velocity);
                    EXPECT_EQ(actual[i].track, 15);
                    EXPECT_FLOAT_EQ(actual[i].gain_mul, expected[i].gain_mul);
                    EXPECT_FLOAT_EQ(actual[i].pitch_ratio_mul, expected[i].pitch_ratio_mul);
                    EXPECT_EQ(actual[i].start_frame, expected[i].start_frame);
                    EXPECT_EQ(actual[i].sample_rate_hz, expected[i].sample_rate_hz);
                    EXPECT_EQ(actual[i].fade_out_ms, expected[i].fade_out_ms);
                    EXPECT_FLOAT_EQ(actual[i].filter_cutoff_hz, expected[i].filter_cutoff_hz);
                }
            }
        }
    }
}
TEST_F(SequencerVoiceMapTest, MissingSamplesAndLayerLimitsPreserveZoneOrder) {
    for (auto& z: instrument.zones) {
        z.key_lo = 0;
        z.key_hi = 127;
        z.vel_lo = 1;
        z.vel_hi = 127;
    }
    instrument.zones[0].sample_id = 0;
    map.PrepareTrack(0, instrument, resolver);
    VoiceTriggerParams out[4];
    EXPECT_EQ(map.Resolve(0, 60, 100, out), 4);
    EXPECT_EQ(out[0].sample, samples[1]);
    EXPECT_EQ(map.Resolve(0, 60, 100, out, 2), 2);
    EXPECT_EQ(map.Resolve(0, 60, 0, out), 0);
    EXPECT_EQ(map.Resolve(16, 60, 100, out), 0);
    EXPECT_EQ(map.Resolve(0, 128, 100, out), 0);
}
TEST_F(SequencerVoiceMapTest, RevocationAndReplacementDoNotMutateAcquiredReferences) {
    for (uint8_t t: {uint8_t{0}, uint8_t{15}})
        map.PrepareTrack(t, instrument, resolver);
    SnapshotMailbox<SequencerVoiceMap> mailbox;
    mailbox.Init(map);
    const auto& old = mailbox.ConsumerValue();
    map.Revoke(1u << 15);
    mailbox.Publish(map);
    EXPECT_GT(old.tracks[15].count, 0);
    ASSERT_TRUE(mailbox.AcquireLatest());
    EXPECT_EQ(mailbox.ConsumerValue().tracks[15].count, 0);
    EXPECT_GT(mailbox.ConsumerValue().tracks[0].count, 0);
    map.PrepareTrack(15, instrument, resolver);
    mailbox.Publish(map);
    EXPECT_EQ(mailbox.ConsumerValue().tracks[15].count, 0);
    ASSERT_TRUE(mailbox.AcquireLatest());
    EXPECT_GT(mailbox.ConsumerValue().tracks[15].count, 0);
    map.Revoke(0xFFFFu);
    for (const auto& track: map.tracks)
        EXPECT_EQ(track.count, 0);
}
TEST_F(SequencerVoiceMapTest, UnboundTrackAndForegroundEditsCannotLeakIntoPublishedState) {
    map.PrepareTrack(2, instrument, resolver);
    VoiceTriggerParams before[4], after[4];
    auto count = map.Resolve(2, 60, 100, before);
    ASSERT_GT(count, 0);
    instrument = Instrument{};
    EXPECT_EQ(map.Resolve(2, 60, 100, after), count);
    EXPECT_EQ(after[0].sample, before[0].sample);
    map.PrepareTrack(2, instrument, resolver);
    EXPECT_EQ(map.Resolve(2, 60, 100, after), 0);
}
}  // namespace

TEST(SequencerVoiceMapCopy, CopiesOnlyLiveCapacityAndRevocationsCannotResurrectZones) {
    WaveX::AudioEngine::SequencerVoiceMap source, dest;
    source.tracks[0].count = 1;
    source.tracks[0].keys[0] = {60, 60, 1, 127, 0, true};
    source.tracks[0].zones[0].root_note = 64;
    dest.tracks[0].zones[31].root_note = 99;
    dest.CopyLiveFrom(source);
    EXPECT_EQ(dest.tracks[0].zones[31].root_note, 99);  // unused slots were not copied
    WaveX::AudioEngine::VoiceTriggerParams out[4];
    ASSERT_EQ(dest.Resolve(0, 60, 100, out), 1);
    EXPECT_EQ(out[0].note, 64);
    source.Revoke(1);
    dest.CopyLiveFrom(source);
    EXPECT_EQ(dest.Resolve(0, 60, 100, out), 0);
}

TEST_F(SequencerVoiceMapTest, SparseTrackEditsAndRevocationsSurviveMailboxReuse) {
    SnapshotMailbox<SequencerVoiceMap> mailbox;
    float expected[16]{};
    for (uint8_t track = 0; track < 16; ++track) {
        instrument.filter.cutoff_hz = expected[track] = 1000.0f + track;
        map.PrepareTrack(track, instrument, resolver);
    }
    mailbox.Init(map);
    VoiceTriggerParams out[4];
    for (uint8_t edit = 0; edit < 64; ++edit) {
        const uint8_t track = edit % 16;
        instrument.filter.cutoff_hz = expected[track] = 2000.0f + edit;
        map.PrepareTrack(track, instrument, resolver);
        mailbox.ProducerValue().CopyLiveFrom(map);
        mailbox.PublishPrepared();
        // Exercise coalescing and all three slot ownership rotations.
        if (edit % 3)
            continue;
        ASSERT_TRUE(mailbox.AcquireLatest());
        for (uint8_t check = 0; check < 16; ++check) {
            ASSERT_GT(mailbox.ConsumerValue().Resolve(check, 0, 75, out), 0);
            EXPECT_FLOAT_EQ(out[0].filter_cutoff_hz, expected[check]);
        }
    }
    map.Revoke(1u << 15);
    mailbox.ProducerValue().CopyLiveFrom(map);
    mailbox.PublishPrepared();
    ASSERT_TRUE(mailbox.AcquireLatest());
    EXPECT_EQ(mailbox.ConsumerValue().Resolve(15, 0, 75, out), 0);
    for (uint8_t check = 0; check < 15; ++check) {
        ASSERT_GT(mailbox.ConsumerValue().Resolve(check, 0, 75, out), 0);
        EXPECT_FLOAT_EQ(out[0].filter_cutoff_hz, expected[check]);
    }
}
