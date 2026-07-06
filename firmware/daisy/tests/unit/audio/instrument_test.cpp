#include "audio/instrument.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

using namespace WaveX::AudioEngine;

namespace {

// A tiny fake sample bank keyed by sample_id. Two frames minimum so
// SampleRef::valid() passes and VoiceManager would accept it.
struct FakeSampleBank {
    std::array<int16_t, 8> data{{100, 100, 100, 100, 100, 100, 100, 100}};

    static SampleRef Resolve(const void* ctx, uint16_t sample_id) {
        auto* self = static_cast<const FakeSampleBank*>(ctx);
        if (sample_id == 0)
            return SampleRef{};  // id 0 = "no sample loaded"
        SampleRef r;
        r.data = self->data.data();
        r.frames = 4;
        r.channels = 1;
        r.sample_rate_hz = 44100;
        return r;
    }

    SampleResolver Resolver() const { return SampleResolver{this, &Resolve}; }
};

Zone MakeZone(uint16_t sample_id,
              uint8_t key_lo,
              uint8_t key_hi,
              uint8_t vel_lo,
              uint8_t vel_hi,
              uint8_t root = 60) {
    Zone z;
    z.sample_id = sample_id;
    z.key_lo = key_lo;
    z.key_hi = key_hi;
    z.vel_lo = vel_lo;
    z.vel_hi = vel_hi;
    z.root_note = root;
    z.in_use = true;
    return z;
}

}  // namespace

TEST(InstrumentTest, UnusedZonesNeverMatch) {
    Instrument ins;  // all zones in_use=false by default
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers), 0);
}

TEST(InstrumentTest, SingleZoneInRangeMatches) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 48, 72, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 5, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(out[0].note, 60);
    EXPECT_EQ(out[0].velocity, 100);
    EXPECT_EQ(out[0].root_note, 60);
    EXPECT_EQ(out[0].slot, 5);
    EXPECT_EQ(out[0].sample_frames, 4u);
    EXPECT_EQ(out[0].sample_rate_hz, 44100u);
}

TEST(InstrumentTest, NoteOutOfKeyRangeDoesNotMatch) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 48, 72, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];
    EXPECT_EQ(ResolveNoteOn(ins, 0, 40, 100, bank.Resolver(), out, kMaxLayerTriggers), 0);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 80, 100, bank.Resolver(), out, kMaxLayerTriggers), 0);
}

// Non-overlapping velocity ranges = hard velocity switching (E-mu primary/
// secondary). Only the matching layer fires.
TEST(InstrumentTest, VelocitySwitchSelectsOneZone) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 0, 127, 1, 63);    // soft
    ins.zones[1] = MakeZone(2, 0, 127, 64, 127);  // hard
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t soft = ResolveNoteOn(ins, 0, 60, 30, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(soft, 1);

    uint8_t hard = ResolveNoteOn(ins, 0, 60, 120, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(hard, 1);
}

// Overlapping ranges layer: both fire (bounded by kMaxLayerTriggers).
TEST(InstrumentTest, OverlappingZonesLayer) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 0, 127, 1, 127);
    ins.zones[1] = MakeZone(2, 0, 127, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    EXPECT_EQ(n, 2);
}

TEST(InstrumentTest, LayerCountIsCappedByMax) {
    Instrument ins;
    for (uint8_t z = 0; z < 6; ++z)
        ins.zones[z] = MakeZone(1, 0, 127, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    // Six matching zones, but the resolver caps at kMaxLayerTriggers (4).
    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    EXPECT_EQ(n, kMaxLayerTriggers);

    // And a caller-supplied smaller cap is honored.
    uint8_t n2 = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, 2);
    EXPECT_EQ(n2, 2);
}

TEST(InstrumentTest, ZoneWithUnresolvableSampleIsSkipped) {
    Instrument ins;
    ins.zones[0] = MakeZone(0, 0, 127, 1, 127);  // sample_id 0 => resolver returns invalid
    ins.zones[1] = MakeZone(1, 0, 127, 1, 127);  // valid
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    // Only the valid zone produces a trigger; the invalid one doesn't consume
    // a slot.
    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    EXPECT_EQ(n, 1);
}

// Drum mode: note is forced to root (no pitch tracking), so every pad plays
// its sample at recorded pitch regardless of which key triggered it.
TEST(InstrumentTest, DrumModeForcesRootNote) {
    Instrument ins;
    ins.mode = InstrumentMode::Drum;
    ins.zones[0] = MakeZone(1, 36, 36, 1, 127, /*root=*/60);  // pad at key 36
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 36, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 1);
    // note forced to root => 12-TET ratio 1.0 (no pitch tracking).
    EXPECT_EQ(out[0].note, 60);
    EXPECT_EQ(out[0].root_note, 60);
}

TEST(InstrumentTest, KeyboardModeKeepsIncomingNote) {
    Instrument ins;
    ins.mode = InstrumentMode::Keyboard;
    ins.zones[0] = MakeZone(1, 0, 127, 1, 127, /*root=*/60);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 67, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(out[0].note, 67);  // pitch-tracks
    EXPECT_EQ(out[0].root_note, 60);
}

TEST(InstrumentTest, TuneRatioFoldsCoarseAndFine) {
    // +12 semitones = one octave = ratio 2.0.
    EXPECT_NEAR(TuneRatio(12, 0), 2.0f, 1e-5f);
    // -12 = half.
    EXPECT_NEAR(TuneRatio(-12, 0), 0.5f, 1e-5f);
    // +100 cents = +1 semitone.
    EXPECT_NEAR(TuneRatio(0, 100), std::pow(2.0f, 1.0f / 12.0f), 1e-5f);
    // identity.
    EXPECT_NEAR(TuneRatio(0, 0), 1.0f, 1e-6f);
}

TEST(InstrumentTest, ZoneTuneFlowsIntoPitchRatioMul) {
    Instrument ins;
    Zone z = MakeZone(1, 0, 127, 1, 127);
    z.coarse_tune = 12;  // +1 octave
    ins.zones[0] = z;
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 1);
    EXPECT_NEAR(out[0].pitch_ratio_mul, 2.0f, 1e-5f);
}

TEST(InstrumentTest, ZoneGainFlowsIntoGainMul) {
    Instrument ins;
    Zone z = MakeZone(1, 0, 127, 1, 127);
    z.gain = 0.25f;
    ins.zones[0] = z;
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 1);
    EXPECT_FLOAT_EQ(out[0].gain_mul, 0.25f);  // no xfade flag => just zone gain
}

// Velocity crossfade up: gain rises across the span. At vel_lo it's 1/span,
// at vel_hi it's 1.0.
TEST(InstrumentTest, VelocityCrossfadeUpRamps) {
    Zone z = MakeZone(1, 0, 127, 1, 100);
    z.flags = ZONE_FLAG_VEL_XFADE;
    EXPECT_NEAR(VelocityXfadeGain(z, 100), 1.0f, 1e-5f);         // top of span
    EXPECT_NEAR(VelocityXfadeGain(z, 1), 1.0f / 100.0f, 1e-5f);  // bottom
    EXPECT_GT(VelocityXfadeGain(z, 80), VelocityXfadeGain(z, 40));
}

TEST(InstrumentTest, VelocityCrossfadeDownRamps) {
    Zone z = MakeZone(1, 0, 127, 1, 100);
    z.flags = ZONE_FLAG_VEL_XFADE_DOWN;
    EXPECT_NEAR(VelocityXfadeGain(z, 1), 1.0f, 1e-5f);  // bottom of span = loudest
    EXPECT_LT(VelocityXfadeGain(z, 80), VelocityXfadeGain(z, 40));
}

// Two overlapping zones with opposite ramps sum toward a smooth crossfade
// (both fire; their combined gains vary continuously with velocity).
TEST(InstrumentTest, OppositeCrossfadeZonesBlend) {
    Instrument ins;
    Zone up = MakeZone(1, 0, 127, 1, 127);
    up.flags = ZONE_FLAG_VEL_XFADE;
    Zone down = MakeZone(2, 0, 127, 1, 127);
    down.flags = ZONE_FLAG_VEL_XFADE_DOWN;
    ins.zones[0] = up;
    ins.zones[1] = down;
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 60, 64, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 2);
    // Both non-zero at a mid velocity; the up-zone louder above center is a
    // separate check - here just confirm both layers are audible.
    EXPECT_GT(out[0].gain_mul, 0.0f);
    EXPECT_GT(out[1].gain_mul, 0.0f);
}

TEST(InstrumentTest, ChokeGroupAndRegionFlowThrough) {
    Instrument ins;
    Zone z = MakeZone(1, 0, 127, 1, 127);
    z.choke_group = 3;
    z.start_frame = 2;
    z.end_frame = 4;
    z.loop_mode = 1;
    z.loop_start = 2;
    z.loop_end = 4;
    z.cutoff_hz = 800.0f;
    z.pan = 0.25f;
    ins.zones[0] = z;
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(out[0].choke_group, 3);
    EXPECT_EQ(out[0].start_frame, 2u);
    EXPECT_EQ(out[0].end_frame, 4u);
    EXPECT_TRUE(out[0].loop);
    EXPECT_FLOAT_EQ(out[0].filter_cutoff_hz, 800.0f);
    EXPECT_FLOAT_EQ(out[0].pan, 0.25f);
}

// InstrumentBank routes a slot to its instrument and resolves through it.
TEST(InstrumentTest, BankResolvesThroughSlot) {
    InstrumentBank bank;
    bank.Slot(4).zones[0] = MakeZone(1, 0, 127, 1, 127);
    FakeSampleBank samples;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = bank.ResolveNote(4, 60, 100, samples.Resolver(), out, kMaxLayerTriggers);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(out[0].slot, 4);

    // A different, empty slot resolves nothing.
    EXPECT_EQ(bank.ResolveNote(5, 60, 100, samples.Resolver(), out, kMaxLayerTriggers), 0);
}

TEST(InstrumentTest, BankSlotOutOfRangeIsSafe) {
    InstrumentBank bank;
    FakeSampleBank samples;
    VoiceTriggerParams out[kMaxLayerTriggers];
    EXPECT_EQ(bank.ResolveNote(200, 60, 100, samples.Resolver(), out, kMaxLayerTriggers), 0);
}
