#include "audio/sequencer_voice_map.hpp"

#include <gtest/gtest.h>

#include "audio/snapshot_mailbox.hpp"
#include "sequencer/sequencer_scheduler.hpp"

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
            auto& z = instrument.osc[0].zones[i];
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

TEST_F(SequencerVoiceMapTest, PreparedTriggersCarryIndependentInstrumentEnvelopes) {
    instrument.env[1] = {.125f, .25f, .5f, .75f};
    instrument.env[2] = {.5f, .75f, .25f, 1.5f};
    map.PrepareTrack(0, instrument, resolver);
    VoiceTriggerParams actual[4];
    const auto count = map.Resolve(0, 60, 100, actual);
    ASSERT_GT(count, 0);
    EXPECT_FLOAT_EQ(actual[0].filter_env_attack_s, .125f);
    EXPECT_FLOAT_EQ(actual[0].filter_env_sustain_level, .5f);
    EXPECT_FLOAT_EQ(actual[0].aux_env_attack_s, .5f);
    EXPECT_FLOAT_EQ(actual[0].aux_env_release_s, 1.5f);
}
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
    for (auto& z: instrument.osc[0].zones) {
        z.key_lo = 0;
        z.key_hi = 127;
        z.vel_lo = 1;
        z.vel_hi = 127;
    }
    instrument.osc[0].zones[0].sample_id = 0;
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

TEST_F(SequencerVoiceMapTest, IndexedDrumPadsMatchLiveResolutionAndRetireAcrossMailboxCopies) {
    for (auto& zone: instrument.osc[0].zones)
        zone = {};
    instrument.mode = InstrumentMode::Drum;
    for (uint8_t i = 0; i < 16; ++i) {
        auto& zone = instrument.osc[0].zones[i];
        zone.in_use = i != 3 && i != 7;
        zone.sample_id = i + 1;
        zone.key_lo = zone.key_hi = 60 + (i * 7) % 16;
        zone.vel_lo = 20;
        zone.vel_hi = 100;
        zone.flags = ZONE_FLAG_VEL_XFADE;
    }
    auto compare = [&]() {
        SequencerVoiceMap copied;
        copied.CopyLiveFrom(map);
        for (uint8_t note = 0; note < 128; ++note)
            for (uint8_t velocity = 1; velocity < 128; ++velocity) {
                VoiceTriggerParams expected[4], actual[4];
                const auto count =
                    ResolveNoteOn(instrument, 15, note, velocity, resolver, expected, 4);
                ASSERT_EQ(copied.Resolve(15, note, velocity, actual), count);
                for (uint8_t i = 0; i < count; ++i) {
                    EXPECT_EQ(actual[i].sample, expected[i].sample);
                    EXPECT_EQ(actual[i].note, expected[i].note);
                    EXPECT_EQ(actual[i].trigger_note, expected[i].trigger_note);
                    EXPECT_EQ(actual[i].velocity, expected[i].velocity);
                    EXPECT_FLOAT_EQ(actual[i].gain_mul, expected[i].gain_mul);
                }
            }
        map.Revoke(1u << 15);
        copied.CopyLiveFrom(map);
        VoiceTriggerParams out[4];
        EXPECT_EQ(copied.Resolve(15, 60, 75, out), 0);
    };
    map.PrepareTrack(15, instrument, resolver);
    ASSERT_TRUE(map.tracks[15].direct_drum);
    compare();
    // Overlapping velocity layers must fall back and preserve their order.
    instrument.osc[0].zones[1].key_lo = instrument.osc[0].zones[1].key_hi = 60;
    map.PrepareTrack(15, instrument, resolver);
    ASSERT_FALSE(map.tracks[15].direct_drum);
    compare();
    instrument.osc[0].zones[1].key_lo = 59;
    map.PrepareTrack(15, instrument, resolver);
    ASSERT_FALSE(map.tracks[15].direct_drum);
    compare();
}

TEST_F(SequencerVoiceMapTest, DualMapsPairInOrderWithIndependentTuningAndGain) {
    instrument.osc[1].type = OscType::Sample;
    instrument.osc[1].keytrack = 0;
    instrument.osc[1].coarse_tune = 12;
    instrument.osc_mix = 0.25f;
    instrument.trim_gain = 0.8f;
    for (uint8_t i = 0; i < 5; ++i) {
        instrument.osc[1].zones[i] = instrument.osc[0].zones[i];
        instrument.osc[1].zones[i].key_lo = 0;
        instrument.osc[1].zones[i].key_hi = 127;
        instrument.osc[1].zones[i].sample_id = 32 - i;
    }
    map.PrepareTrack(0, instrument, resolver);
    for (uint8_t note = 0; note < 128; ++note) {
        for (uint8_t vel: {uint8_t{1}, uint8_t{60}, uint8_t{127}}) {
            VoiceTriggerParams live[4], prepared[4];
            auto count = ResolveNoteOn(instrument, 0, note, vel, resolver, live, 4);
            ASSERT_EQ(count, map.Resolve(0, note, vel, prepared));
            for (uint8_t i = 0; i < count; ++i) {
                EXPECT_EQ(live[i].sample, prepared[i].sample);
                EXPECT_EQ(live[i].note, prepared[i].note);
                EXPECT_FLOAT_EQ(live[i].source_level, prepared[i].source_level);
                EXPECT_FLOAT_EQ(live[i].gain_mul, prepared[i].gain_mul);
                EXPECT_EQ(live[i].secondary.sample, prepared[i].secondary.sample);
                EXPECT_EQ(live[i].secondary.note, prepared[i].secondary.note);
                EXPECT_FLOAT_EQ(live[i].secondary.pitch_ratio_mul,
                                prepared[i].secondary.pitch_ratio_mul);
                EXPECT_FLOAT_EQ(live[i].secondary.source_level, prepared[i].secondary.source_level);
            }
        }
    }
    VoiceTriggerParams out[4];
    ASSERT_EQ(map.Resolve(0, 0, 60, out), 4);
    EXPECT_EQ(out[0].sample, samples[0]);
    EXPECT_EQ(out[0].secondary.sample, samples[31]);
    EXPECT_EQ(out[1].sample, samples[30]);  // no second primary match: secondary alone
    EXPECT_EQ(out[1].secondary.sample, nullptr);
    SequencerVoiceMap copy;
    copy.CopyLiveFrom(map);
    map.Revoke(1);
    copy.CopyLiveFrom(map);
    EXPECT_EQ(copy.Resolve(0, 0, 60, out), 0);
}

TEST_F(SequencerVoiceMapTest, DisabledSourcesDoNotResolveStaleZones) {
    instrument.osc[1] = instrument.osc[0];
    instrument.osc[0].type = OscType::Off;
    instrument.osc_mix = 1;
    map.PrepareTrack(0, instrument, resolver);
    VoiceTriggerParams out[4];
    ASSERT_GT(map.Resolve(0, 0, 70, out), 0);
    EXPECT_EQ(out[0].secondary.sample, nullptr);
    instrument.osc[1].type = OscType::Wavetable;
    map.PrepareTrack(0, instrument, resolver);
    EXPECT_EQ(map.Resolve(0, 0, 70, out), 0);
}

TEST(SequencerVoiceAlignment, EightTracksKeepIdenticalSamplePhaseAcrossRepeatedSteals) {
    // Exercise the scheduler -> prepared Instrument -> VoiceManager path with
    // all voice slots occupied. 123 BPM also puts triggers inside audio blocks.
    for (const float bpm: {120.f, 123.f}) {
        SCOPED_TRACE(bpm);
        constexpr uint32_t block_size = 48;
        int16_t pcm[256];
        for (size_t i = 0; i < 256; ++i)
            pcm[i] = static_cast<int16_t>(i * 64);
        SampleResolver resolver{pcm, [](const void* ctx, uint16_t) {
                                    SampleRef ref;
                                    ref.data = static_cast<const int16_t*>(ctx);
                                    ref.frames = 256;
                                    ref.sample_rate_hz = 44100;
                                    ref.loop_enabled = true;
                                    ref.loop_end = 256;
                                    return ref;
                                }};
        Instrument instrument;
        instrument.origin = InstrumentOrigin::Built;
        auto& zone = instrument.osc[0].zones[0];
        zone.in_use = true;
        zone.sample_id = 1;
        zone.key_lo = zone.key_hi = zone.root_note = 60;
        zone.vel_lo = 1;
        zone.vel_hi = 127;
        SequencerVoiceMap map;
        WaveX::Sequencer::Pattern pattern;
        pattern.length = 16;
        pattern.scale = WaveX::Sequencer::StepScale::Sixteenth;
        pattern.swing = 50;
        for (uint8_t track = 0; track < 16; ++track) {
            pattern.tracks[track].enabled = track < 8;
            if (track >= 8)
                continue;
            map.PrepareTrack(track, instrument, resolver);
            for (uint8_t step = 0; step < 16; step += 2)
                pattern.tracks[track].steps[step].on = true;
        }
        WaveX::Sequencer::SequencerScheduler scheduler;
        scheduler.Init(48000, block_size);
        scheduler.SetTempo(bpm);
        scheduler.SetPattern(&pattern);
        scheduler.Start();
        VoiceManager voices;
        voices.Init(48000);
        float left[block_size], right[block_size];
        uint32_t groups = 0, nonzero_offsets = 0;
        for (uint32_t block = 0; block < 8000; ++block) {
            const auto start = scheduler.CurrentFrame();
            WaveX::Sequencer::TriggerEvent events[64];
            const auto count = scheduler.Process(events, 64);
            if (count) {
                ASSERT_EQ(count, 8u);
                const auto expected_frame =
                    static_cast<uint64_t>(48000.0 * 30.0 * groups / bpm + .5);
                for (uint8_t track = 0; track < 8; ++track) {
                    ASSERT_EQ(events[track].track, track);
                    ASSERT_EQ(events[track].frame, expected_frame);
                }
                SequencerVoiceMap::Selection selections[8];
                voices.TriggerBatch(
                    8,
                    [&](uint16_t request) {
                        const auto& event = events[request];
                        selections[request] = map.Select(event.track, 60, 100);
                        EXPECT_EQ(selections[request].count, 1);
                        return map.Describe(selections[request]);
                    },
                    [&](uint16_t request, uint8_t layer, VoiceTriggerParams& trigger) {
                        map.Materialize(selections[request], layer, trigger);
                        trigger.start_offset_frames =
                            static_cast<uint16_t>(events[request].frame - start);
                    });
                nonzero_offsets += expected_frame != start;
                ++groups;
            }
            voices.Render(left, right, block_size);
            ASSERT_EQ(voices.ActiveVoiceCount(), 8);
            uint16_t tracks = 0;
            const auto& first = voices.GetVoice(0);
            for (uint8_t index = 0; index < 8; ++index) {
                const auto& voice = voices.GetVoice(index);
                tracks |= 1u << voice.track;
                ASSERT_EQ(voice.phase.Frame(), first.phase.Frame());
                ASSERT_FLOAT_EQ(voice.phase.Fraction(), first.phase.Fraction());
            }
            ASSERT_EQ(tracks, 0xFF);
        }
        EXPECT_GE(groups, 32u);
        if (bpm != 120.f) {
            EXPECT_GT(nonzero_offsets, 0u);
        }
    }
}

TEST_F(SequencerVoiceMapTest, AdmissionMetadataMatchesDualSourceChannelCostAndPrimaryChoke) {
    SampleResolver stereo{&resolver, [](const void* context, uint16_t id) {
                              auto sample = static_cast<const SampleResolver*>(context)->Get(id);
                              sample.channels = id % 2 ? 1 : 2;
                              return sample;
                          }};
    instrument.osc[1] = instrument.osc[0];
    for (auto& zone: instrument.osc[1].zones)
        zone.sample_id = zone.sample_id == 32 ? 1 : zone.sample_id + 1;
    for (auto& zone: instrument.osc[0].zones)
        zone.choke_group = 3;
    for (auto& zone: instrument.osc[1].zones)
        zone.choke_group = 7;
    map.PrepareTrack(2, instrument, stereo);
    for (uint8_t note = 0; note < 128; ++note) {
        const auto selected = map.Select(2, note, 70);
        const auto description = map.Describe(selected);
        VoiceTriggerParams expected[4];
        ASSERT_EQ(description.count, ResolveNoteOn(instrument, 2, note, 70, stereo, expected, 4));
        for (uint8_t i = 0; i < description.count; ++i) {
            const auto& p = expected[i];
            const bool primary_stereo = p.channels == 2 && !p.mono;
            const bool secondary_stereo =
                p.secondary.sample && p.secondary.channels == 2 && !p.secondary.mono;
            EXPECT_EQ(description.layers[i].channels, primary_stereo || secondary_stereo ? 2 : 1);
            EXPECT_EQ(description.layers[i].choke, p.choke_group);
        }
    }
}
