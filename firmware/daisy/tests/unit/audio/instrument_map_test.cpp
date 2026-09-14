#include "audio/instrument_map.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace WaveX::AudioEngine;
namespace IM = WaveX::AudioEngine::InstrumentMap;
namespace Wxi = WaveX::Wxi;

namespace {

// Names every sample "/samples/<id>.wav" so a test can tell which sample a
// zone referred to without a Pool behind it.
bool PathForId(const void* /*ctx*/, uint16_t sample_id, char* out, size_t out_len) {
    std::snprintf(out, out_len, "/samples/%u.wav", static_cast<unsigned>(sample_id));
    return true;
}

IM::SamplePathResolver Resolver() {
    return IM::SamplePathResolver{nullptr, &PathForId};
}

Zone MakeZone(uint16_t sample_id, uint8_t key_lo, uint8_t key_hi) {
    Zone z;
    z.sample_id = sample_id;
    z.key_lo = key_lo;
    z.key_hi = key_hi;
    z.in_use = true;
    return z;
}

}  // namespace

TEST(InstrumentMapTest, NameModeFilterAndEnvelopeReachTheDocument) {
    Instrument ins;
    std::strncpy(ins.name, "Rhodes Mk I", sizeof(ins.name) - 1);
    ins.mode = InstrumentMode::Drum;
    ins.filter.type = 2;
    ins.filter.topology = WaveX::Protocol::INST_FILTER_TOPOLOGY_LADDER;
    ins.filter.slope = WaveX::Protocol::INST_FILTER_SLOPE_24;
    ins.filter.drive = 0.7f;
    ins.filter.cutoff_hz = 1234.5f;
    ins.filter.resonance = 0.42f;
    ins.env[0].attack_s = 0.11f;
    ins.env[0].decay_s = 0.22f;
    ins.env[0].sustain = 0.33f;
    ins.env[0].release_s = 0.44f;

    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    EXPECT_STREQ(doc.name, "Rhodes Mk I");
    EXPECT_EQ(doc.mode, Wxi::Mode::Drum);
    EXPECT_EQ(doc.filter.type, Wxi::FilterType::SvfBp);
    EXPECT_EQ(doc.filter.topology, Wxi::FilterTopology::Ladder);
    EXPECT_EQ(doc.filter.slope, Wxi::FilterSlope::Db24);
    EXPECT_FLOAT_EQ(doc.filter.drive, 0.7f);
    EXPECT_FLOAT_EQ(doc.filter.cutoff_hz, 1234.5f);
    EXPECT_FLOAT_EQ(doc.filter.resonance, 0.42f);
    EXPECT_FLOAT_EQ(doc.env[0].attack_s, 0.11f);
    EXPECT_FLOAT_EQ(doc.env[0].decay_s, 0.22f);
    EXPECT_FLOAT_EQ(doc.env[0].sustain, 0.33f);
    EXPECT_FLOAT_EQ(doc.env[0].release_s, 0.44f);
}

// The reason the stored `index` exists. An Instrument whose live zones are
// 3, 9 and 30 must come back with them still at 3, 9 and 30 - an editor that
// addresses zones by index (the Pad Map: zone index IS the pad) would
// otherwise renumber the user's kit on every save.
TEST(InstrumentMapTest, SparseZoneIndicesSurviveTheRoundTrip) {
    Instrument ins;
    ins.osc[0].zones[3] = MakeZone(11, 0, 40);
    ins.osc[0].zones[9] = MakeZone(22, 41, 80);
    ins.osc[0].zones[30] = MakeZone(33, 81, 127);

    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    ASSERT_EQ(doc.osc[IM::kSampleOsc].zone_count, 3);
    EXPECT_EQ(doc.osc[IM::kSampleOsc].zones[0].index, 3);
    EXPECT_EQ(doc.osc[IM::kSampleOsc].zones[1].index, 9);
    EXPECT_EQ(doc.osc[IM::kSampleOsc].zones[2].index, 30);

    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);
    EXPECT_TRUE(back.instrument.osc[0].zones[3].in_use);
    EXPECT_TRUE(back.instrument.osc[0].zones[9].in_use);
    EXPECT_TRUE(back.instrument.osc[0].zones[30].in_use);
    EXPECT_FALSE(back.instrument.osc[0].zones[0].in_use);
    EXPECT_FALSE(back.instrument.osc[0].zones[4].in_use);
    EXPECT_EQ(back.instrument.osc[0].zones[3].key_hi, 40);
    EXPECT_EQ(back.instrument.osc[0].zones[9].key_hi, 80);
    EXPECT_EQ(back.instrument.osc[0].zones[30].key_hi, 127);
    EXPECT_STREQ(back.sample_paths[3], "/samples/11.wav");
    EXPECT_STREQ(back.sample_paths[9], "/samples/22.wav");
    EXPECT_STREQ(back.sample_paths[30], "/samples/33.wav");
}

// Every zone field, both directions: the seam between the two models is
// exactly where a field gets quietly dropped.
TEST(InstrumentMapTest, EveryZoneFieldSurvivesBothDirections) {
    Instrument ins;
    Zone z = MakeZone(7, 36, 48);
    z.vel_lo = 20;
    z.vel_hi = 110;
    z.root_note = 40;
    z.coarse_tune = -5;
    z.fine_tune = 33;
    z.gain = 0.75f;
    z.pan = 0.25f;
    z.start_frame = 100;
    z.end_frame = 2000;
    z.loop_start = 300;
    z.loop_end = 1500;
    z.loop_mode = ZONE_LOOP_FORWARD;
    z.choke_group = 3;
    z.output_bus = 1;
    z.flags = ZONE_FLAG_OWN_FILTER_ENV | ZONE_FLAG_ONE_SHOT;
    z.cutoff_hz = 880.0f;
    z.attack_s = 0.05f;
    z.decay_s = 0.15f;
    z.sustain = 0.6f;
    z.release_s = 0.25f;
    ins.osc[0].zones[5] = z;

    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);

    const Zone& r = back.instrument.osc[0].zones[5];
    EXPECT_EQ(r.key_lo, 36);
    EXPECT_EQ(r.key_hi, 48);
    EXPECT_EQ(r.vel_lo, 20);
    EXPECT_EQ(r.vel_hi, 110);
    EXPECT_EQ(r.root_note, 40);
    EXPECT_EQ(r.coarse_tune, -5);
    EXPECT_EQ(r.fine_tune, 33);
    EXPECT_FLOAT_EQ(r.gain, 0.75f);
    EXPECT_FLOAT_EQ(r.pan, 0.25f);
    EXPECT_EQ(r.start_frame, 100u);
    EXPECT_EQ(r.end_frame, 2000u);
    EXPECT_EQ(r.loop_start, 300u);
    EXPECT_EQ(r.loop_end, 1500u);
    EXPECT_EQ(r.loop_mode, ZONE_LOOP_FORWARD);
    EXPECT_EQ(r.choke_group, 3);
    EXPECT_EQ(r.output_bus, 1);
    EXPECT_EQ(r.flags, ZONE_FLAG_OWN_FILTER_ENV | ZONE_FLAG_ONE_SHOT);
    EXPECT_FLOAT_EQ(r.cutoff_hz, 880.0f);
    EXPECT_FLOAT_EQ(r.attack_s, 0.05f);
    EXPECT_FLOAT_EQ(r.decay_s, 0.15f);
    EXPECT_FLOAT_EQ(r.sustain, 0.6f);
    EXPECT_FLOAT_EQ(r.release_s, 0.25f);
    EXPECT_TRUE(r.in_use);
}

// A stored Zone names its Sample by path; the engine names it by Pool id.
// Loading must not invent an id - the loader assigns one on admission, and a
// guessed id would resolve to whatever happens to occupy that slot.
TEST(InstrumentMapTest, LoadLeavesSampleIdForTheLoaderToAssign) {
    Instrument ins;
    ins.osc[0].zones[0] = MakeZone(4242, 0, 127);
    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    EXPECT_STREQ(doc.osc[IM::kSampleOsc].zones[0].path, "/samples/4242.wav");

    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);
    EXPECT_EQ(back.instrument.osc[0].zones[0].sample_id, 0);
    EXPECT_STREQ(back.sample_paths[0], "/samples/4242.wav");
}

TEST(InstrumentMapTest, ModMatrixSurvivesBothDirections) {
    Instrument ins;
    ins.mod_slots[0].source = SRC_LFO1;
    ins.mod_slots[0].dest = DEST_CUTOFF;
    ins.mod_slots[0].depth = -12345;
    ins.mod_slots[0].curve = 1;
    ins.mod_slots[0].flags = 2;
    ins.mod_slots[7].depth = 32767;

    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);

    EXPECT_EQ(back.instrument.mod_slots[0].source, SRC_LFO1);
    EXPECT_EQ(back.instrument.mod_slots[0].dest, DEST_CUTOFF);
    EXPECT_EQ(back.instrument.mod_slots[0].depth, -12345);
    EXPECT_EQ(back.instrument.mod_slots[0].curve, 1);
    EXPECT_EQ(back.instrument.mod_slots[0].flags, 2);
    EXPECT_EQ(back.instrument.mod_slots[7].depth, 32767);
    EXPECT_EQ(back.instrument.mod_slots[3].source, SRC_NONE);
}

TEST(InstrumentMapTest, FullZoneComplementSurvives) {
    Instrument ins;
    for (uint8_t z = 0; z < kMaxZones; ++z) {
        ins.osc[0].zones[z] = MakeZone(static_cast<uint16_t>(z + 1), z, z);
    }
    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    EXPECT_EQ(doc.osc[IM::kSampleOsc].zone_count, kMaxZones);

    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);
    for (uint8_t z = 0; z < kMaxZones; ++z) {
        EXPECT_TRUE(back.instrument.osc[0].zones[z].in_use) << "zone " << int(z);
        EXPECT_EQ(back.instrument.osc[0].zones[z].key_lo, z);
        EXPECT_EQ(back.sample_paths[z], "/samples/" + std::to_string(z + 1) + ".wav");
    }
}

// An empty Instrument is still a Sample Instrument - an empty pad map the
// user is about to fill, not an oscillator that has been switched off.
TEST(InstrumentMapTest, EmptyInstrumentIsAnEmptySampleOscillator) {
    Instrument ins;
    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(ins, Resolver(), doc));
    EXPECT_EQ(doc.osc[IM::kSampleOsc].zone_count, 0);
    EXPECT_EQ(doc.osc[IM::kSampleOsc].type, Wxi::OscType::Sample);

    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);
    EXPECT_EQ(back.zone_count, 0);
    EXPECT_FALSE(back.instrument.osc[0].zones[0].in_use);
}

// A save whose Sample cannot be named must fail rather than write a document
// with an unloadable zone.
TEST(InstrumentMapTest, UnnameableSampleFailsTheSave) {
    Instrument ins;
    ins.osc[0].zones[0] = MakeZone(5, 0, 127);
    Wxi::InstrumentFile doc;
    IM::SamplePathResolver refuses{nullptr,
                                   [](const void*, uint16_t, char*, size_t) { return false; }};
    EXPECT_FALSE(IM::ToFile(ins, refuses, doc));
}

// A document from a build with more zones than this one must not wrap a
// stored index onto a good zone.
TEST(InstrumentMapTest, OutOfRangeStoredIndexIsDroppedNotWrapped) {
    Wxi::InstrumentFile doc;
    Wxi::Oscillator& osc = doc.osc[IM::kSampleOsc];
    osc.type = Wxi::OscType::Sample;
    osc.zone_count = 2;
    osc.zones[0].index = 1;
    osc.zones[0].key_hi = 60;
    std::strncpy(osc.zones[0].path, "/keep.wav", sizeof(osc.zones[0].path) - 1);
    osc.zones[1].index = kMaxZones + 4;  // no such zone here
    osc.zones[1].key_hi = 99;
    std::strncpy(osc.zones[1].path, "/drop.wav", sizeof(osc.zones[1].path) - 1);

    Sfz::MappedInstrument back;
    IM::FromFile(doc, back);
    EXPECT_TRUE(back.instrument.osc[0].zones[1].in_use);
    EXPECT_EQ(back.instrument.osc[0].zones[1].key_hi, 60);
    EXPECT_STREQ(back.sample_paths[1], "/keep.wav");
    // Nothing else was written - in particular the out-of-range zone did not
    // land on zone (index % kMaxZones).
    for (uint8_t z = 0; z < kMaxZones; ++z) {
        if (z == 1)
            continue;
        EXPECT_FALSE(back.instrument.osc[0].zones[z].in_use) << "zone " << int(z);
    }
}

// FromFile reuses one resident buffer, so it must not inherit the previous
// load's zones or paths.
TEST(InstrumentMapTest, LoadFullyResetsItsOutput) {
    Instrument first;
    std::strncpy(first.name, "First", sizeof(first.name) - 1);
    first.osc[0].zones[0] = MakeZone(1, 0, 127);
    first.osc[0].zones[7] = MakeZone(2, 0, 127);

    Wxi::InstrumentFile doc;
    ASSERT_TRUE(IM::ToFile(first, Resolver(), doc));
    Sfz::MappedInstrument out;
    IM::FromFile(doc, out);
    ASSERT_TRUE(out.instrument.osc[0].zones[7].in_use);

    Instrument second;
    std::strncpy(second.name, "Second", sizeof(second.name) - 1);
    Wxi::InstrumentFile doc2;
    ASSERT_TRUE(IM::ToFile(second, Resolver(), doc2));
    IM::FromFile(doc2, out);

    EXPECT_STREQ(out.instrument.name, "Second");
    EXPECT_EQ(out.zone_count, 0);
    EXPECT_FALSE(out.instrument.osc[0].zones[0].in_use);
    EXPECT_FALSE(out.instrument.osc[0].zones[7].in_use);
    EXPECT_STREQ(out.sample_paths[7], "");
}

// --- which parser a path gets --------------------------------------------

TEST(InstrumentMapTest, SfzIsRecognisedCaseInsensitively) {
    EXPECT_TRUE(IM::PathIsSfz("/sfz/piano.sfz"));
    EXPECT_TRUE(IM::PathIsSfz("/sfz/PIANO.SFZ"));
    EXPECT_TRUE(IM::PathIsSfz("/sfz/Piano.Sfz"));
    EXPECT_TRUE(IM::PathIsSfz(".sfz"));
}

// Everything else is offered to the .wxi reader, which rejects it by content
// if it really is something else - a better answer than refusing on spelling.
TEST(InstrumentMapTest, EverythingElseGoesToTheWxiReader) {
    EXPECT_FALSE(IM::PathIsSfz("/instruments/rhodes.wxi"));
    EXPECT_FALSE(IM::PathIsSfz("/instruments/RHODES.WXI"));
    EXPECT_FALSE(IM::PathIsSfz("/samples/kick.wav"));
    EXPECT_FALSE(IM::PathIsSfz("/no-extension"));
    EXPECT_FALSE(IM::PathIsSfz("sfz"));  // no dot: a name, not an extension
    EXPECT_FALSE(IM::PathIsSfz(""));
    EXPECT_FALSE(IM::PathIsSfz(nullptr));
}

TEST(InstrumentMapTest, EditingCutoffKeepsExpandedInstrumentSettingsOnSave) {
    Wxi::InstrumentFile saved;
    std::strcpy(saved.name, "Two sources");
    saved.tags = 7;
    saved.transpose = -12;
    saved.fine_tune = 25;
    saved.trim_gain = .75f;
    saved.trim_pan = .25f;
    saved.osc_mix = .4f;
    saved.output = 2;
    saved.poly_mode = 1;
    saved.amp.velocity_curve = 2;
    saved.filter.keytrack = .5f;
    saved.filter.env2_amount = .3f;
    saved.filter.topology = Wxi::FilterTopology::Ladder;
    saved.filter.slope = Wxi::FilterSlope::Db24;
    saved.filter.drive = .25f;
    saved.osc[0].type = Wxi::OscType::Sample;
    saved.osc[1].type = Wxi::OscType::Sample;
    saved.osc[1].level = .6f;
    saved.osc[1].coarse_tune = 7;
    saved.osc[1].fine_tune = -30;
    saved.osc[1].keytrack = 0;
    saved.env[1].decay_s = .9f;
    saved.env[2].attack_s = .8f;
    saved.lfo[1].rate_hz = 5.5f;
    saved.lfo[1].delay_s = .2f;
    saved.lfo[1].fade_s = .7f;
    saved.lfo[1].wave = 3;
    saved.lfo[1].sync_div = 4;
    saved.lfo[1].retrigger = 0;
    Sfz::MappedInstrument loaded;
    IM::FromFile(saved, loaded);
    loaded.instrument.filter.cutoff_hz = 987;
    Wxi::InstrumentFile next;
    ASSERT_TRUE(IM::ToFile(loaded.instrument, Resolver(), next));
    EXPECT_EQ(next.filter.topology, Wxi::FilterTopology::Ladder);
    EXPECT_EQ(next.filter.slope, Wxi::FilterSlope::Db24);
    EXPECT_FLOAT_EQ(next.filter.drive, .25f);
    EXPECT_FLOAT_EQ(next.filter.cutoff_hz, 987);
    EXPECT_EQ(next.tags, 7);
    EXPECT_EQ(next.transpose, -12);
    EXPECT_EQ(next.fine_tune, 25);
    EXPECT_FLOAT_EQ(next.trim_gain, .75f);
    EXPECT_FLOAT_EQ(next.trim_pan, .25f);
    EXPECT_FLOAT_EQ(next.osc_mix, .4f);
    EXPECT_EQ(next.output, 2);
    EXPECT_EQ(next.poly_mode, 1);
    EXPECT_EQ(next.amp.velocity_curve, 2);
    EXPECT_FLOAT_EQ(next.filter.keytrack, .5f);
    EXPECT_FLOAT_EQ(next.filter.env2_amount, .3f);
    EXPECT_EQ(next.osc[1].type, Wxi::OscType::Sample);
    EXPECT_FLOAT_EQ(next.osc[1].level, .6f);
    EXPECT_EQ(next.osc[1].coarse_tune, 7);
    EXPECT_EQ(next.osc[1].fine_tune, -30);
    EXPECT_EQ(next.osc[1].keytrack, 0);
    EXPECT_FLOAT_EQ(next.env[1].decay_s, .9f);
    EXPECT_FLOAT_EQ(next.env[2].attack_s, .8f);
    EXPECT_FLOAT_EQ(next.lfo[1].rate_hz, 5.5f);
    EXPECT_FLOAT_EQ(next.lfo[1].delay_s, .2f);
    EXPECT_FLOAT_EQ(next.lfo[1].fade_s, .7f);
    EXPECT_EQ(next.lfo[1].wave, 3);
    EXPECT_EQ(next.lfo[1].sync_div, 4);
    EXPECT_EQ(next.lfo[1].retrigger, 0);
}
TEST(InstrumentMapTest, SamplePlanDeduplicatesPathsAcrossSparseOscillatorMaps) {
    Wxi::InstrumentFile saved;
    for (auto& osc: saved.osc) {
        osc.type = Wxi::OscType::Sample;
        osc.zone_count = 1;
        osc.zones[0].index = 31;
        std::strcpy(osc.zones[0].path, "/shared.wav");
    }
    Sfz::MappedInstrument mapped;
    IM::FromFile(saved, mapped);
    EXPECT_EQ(mapped.zone_count, 64);
    EXPECT_STREQ(mapped.sample_paths[31], "/shared.wav");
    EXPECT_STREQ(mapped.sample_paths[63], "/shared.wav");
    Sfz::SamplePlan plan;
    Sfz::Status status;
    ASSERT_TRUE(Sfz::BuildSamplePlan(mapped, plan, status));
    EXPECT_EQ(plan.count, 1);
    EXPECT_EQ(mapped.instrument.osc[0].zones[31].sample_id, 1);
    EXPECT_EQ(mapped.instrument.osc[1].zones[31].sample_id, 1);
}
