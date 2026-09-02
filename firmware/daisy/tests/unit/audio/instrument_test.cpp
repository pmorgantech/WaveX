#include "audio/instrument.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

using namespace WaveX::AudioEngine;

namespace {

// A tiny fake sample bank keyed by sample_id. Two frames minimum so
// SampleRef::valid() passes and VoiceManager would accept it. The frame
// count ENCODES the sample_id (frames = 2 + id), so a test can tell from a
// trigger's sample_frames WHICH zone actually resolved it - count-only
// assertions can't distinguish "the right zone fired" from "the wrong zone
// fired".
struct FakeSampleBank {
    std::array<int16_t, 8> data{{100, 100, 100, 100, 100, 100, 100, 100}};

    static SampleRef Resolve(const void* ctx, uint16_t sample_id) {
        auto* self = static_cast<const FakeSampleBank*>(ctx);
        if (sample_id == 0)
            return SampleRef{};  // id 0 = "no sample loaded"
        SampleRef r;
        r.data = self->data.data();
        r.frames = 2u + sample_id;  // identifies the zone that resolved
        r.channels = 1;
        r.sample_rate_hz = 44100;
        return r;
    }

    SampleResolver Resolver() const { return SampleResolver{this, &Resolve}; }
};

// The frame count FakeSampleBank reports for a given sample_id.
uint32_t FramesFor(uint16_t sample_id) {
    return 2u + sample_id;
}

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
    EXPECT_EQ(out[0].sample_frames, FramesFor(1));
    EXPECT_EQ(out[0].sample_rate_hz, 44100u);
}

// Key and velocity ranges are documented INCLUSIVE at both ends - an
// off-by-one on either boundary silences (or doubles) real notes.
TEST(InstrumentTest, KeyAndVelocityBoundariesAreInclusive) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 48, 72, 10, 90);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    // Exactly on each boundary: matches.
    EXPECT_EQ(ResolveNoteOn(ins, 0, 48, 50, bank.Resolver(), out, kMaxLayerTriggers), 1);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 72, 50, bank.Resolver(), out, kMaxLayerTriggers), 1);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 10, bank.Resolver(), out, kMaxLayerTriggers), 1);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 90, bank.Resolver(), out, kMaxLayerTriggers), 1);

    // One outside each boundary: no match.
    EXPECT_EQ(ResolveNoteOn(ins, 0, 47, 50, bank.Resolver(), out, kMaxLayerTriggers), 0);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 73, 50, bank.Resolver(), out, kMaxLayerTriggers), 0);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 9, bank.Resolver(), out, kMaxLayerTriggers), 0);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 91, bank.Resolver(), out, kMaxLayerTriggers), 0);
}

// Velocity 0 sits below the default vel_lo of 1, so a zone left at defaults
// never fires on it (velocity-0 note-ons are note-offs in MIDI).
TEST(InstrumentTest, VelocityZeroDoesNotMatchDefaultZone) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 0, 127, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 0, bank.Resolver(), out, kMaxLayerTriggers), 0);
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
    EXPECT_EQ(out[0].sample_frames, FramesFor(1)) << "velocity 30 must select the SOFT zone";

    uint8_t hard = ResolveNoteOn(ins, 0, 60, 120, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(hard, 1);
    EXPECT_EQ(out[0].sample_frames, FramesFor(2)) << "velocity 120 must select the HARD zone";

    // The switch point itself: 63 is the top of soft, 64 the bottom of hard.
    ASSERT_EQ(ResolveNoteOn(ins, 0, 60, 63, bank.Resolver(), out, kMaxLayerTriggers), 1);
    EXPECT_EQ(out[0].sample_frames, FramesFor(1));
    ASSERT_EQ(ResolveNoteOn(ins, 0, 60, 64, bank.Resolver(), out, kMaxLayerTriggers), 1);
    EXPECT_EQ(out[0].sample_frames, FramesFor(2));
}

// Overlapping ranges layer: both fire (bounded by kMaxLayerTriggers).
TEST(InstrumentTest, OverlappingZonesLayer) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 0, 127, 1, 127);
    ins.zones[1] = MakeZone(2, 0, 127, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    uint8_t n = ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, kMaxLayerTriggers);
    ASSERT_EQ(n, 2);
    // Both zones fired, in zone order, each carrying its own sample.
    EXPECT_EQ(out[0].sample_frames, FramesFor(1));
    EXPECT_EQ(out[1].sample_frames, FramesFor(2));
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
    ASSERT_EQ(n, 1);
    EXPECT_EQ(out[0].sample_frames, FramesFor(1)) << "the trigger must come from the VALID zone";
}

TEST(InstrumentTest, NullOutputOrZeroMaxIsRejected) {
    Instrument ins;
    ins.zones[0] = MakeZone(1, 0, 127, 1, 127);
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), nullptr, kMaxLayerTriggers), 0);
    EXPECT_EQ(ResolveNoteOn(ins, 0, 60, 100, bank.Resolver(), out, 0), 0);
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
    EXPECT_EQ(out[0].trigger_note, 36);
    EXPECT_EQ(out[0].root_note, 60);
}

TEST(InstrumentTest, OneShotFlagFlowsIntoTriggerParams) {
    Instrument ins;
    Zone z = MakeZone(1, 0, 127, 1, 127);
    z.flags = ZONE_FLAG_ONE_SHOT;
    ins.zones[0] = z;
    FakeSampleBank bank;
    VoiceTriggerParams out[kMaxLayerTriggers];

    ASSERT_EQ(ResolveNoteOn(ins, 2, 64, 100, bank.Resolver(), out, kMaxLayerTriggers), 1);
    EXPECT_TRUE(out[0].one_shot);
    EXPECT_EQ(out[0].trigger_note, 64);
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
    // Exact ramp arithmetic over the shared [1,127] span (span = 127):
    // up = velocity/127, down = (128 - velocity)/127. At velocity 64 the two
    // meet at 64/127 each, and at ANY velocity they sum to the constant
    // 128/127 - the property that makes the crossfade level-preserving.
    EXPECT_NEAR(out[0].gain_mul, 64.0f / 127.0f, 1e-5f);
    EXPECT_NEAR(out[1].gain_mul, 64.0f / 127.0f, 1e-5f);
    for (int vel_i: {1, 30, 100, 127}) {
        const uint8_t vel = static_cast<uint8_t>(vel_i);
        uint8_t m = ResolveNoteOn(ins, 0, 60, vel, bank.Resolver(), out, kMaxLayerTriggers);
        ASSERT_EQ(m, 2) << "vel " << int(vel);
        EXPECT_NEAR(out[0].gain_mul + out[1].gain_mul, 128.0f / 127.0f, 1e-5f)
            << "vel " << int(vel);
    }
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

// --- Modulation matrix storage (param-locks-and-modulation.md §9 stage 4) --
//
// mod_slots is a fixed 8-entry array with no separate "populated count" -
// these pin that a fresh Instrument's array is all identity (SRC_NONE/
// DEST_NONE), matching what EvaluateModMatrix() already treats as a no-op,
// and that a slot is addressable and independent of every other slot and
// every other instrument in the bank.

TEST(InstrumentTest, FreshInstrumentHasIdentityModSlots) {
    Instrument ins;
    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        EXPECT_EQ(ins.mod_slots[i].source, SRC_NONE);
        EXPECT_EQ(ins.mod_slots[i].dest, DEST_NONE);
        EXPECT_EQ(ins.mod_slots[i].depth, 0);
    }
}

TEST(InstrumentTest, ModSlotsAreIndependentPerInstrumentAndPerSlotIndex) {
    InstrumentBank bank;
    ModSlot a;
    a.source = SRC_LFO1;
    a.dest = DEST_CUTOFF;
    a.depth = 12345;
    bank.Slot(2).mod_slots[3] = a;

    // A different slot index on the same instrument is untouched.
    EXPECT_EQ(bank.Slot(2).mod_slots[4].source, SRC_NONE);
    // A different instrument slot entirely is untouched.
    EXPECT_EQ(bank.Slot(5).mod_slots[3].source, SRC_NONE);
    // The written entry reads back exactly.
    EXPECT_EQ(bank.Slot(2).mod_slots[3].source, SRC_LFO1);
    EXPECT_EQ(bank.Slot(2).mod_slots[3].dest, DEST_CUTOFF);
    EXPECT_EQ(bank.Slot(2).mod_slots[3].depth, 12345);
}
