#include "audio/sfz_import.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>

using namespace WaveX::AudioEngine;
using namespace WaveX::AudioEngine::Sfz;

namespace {

bool Feed(Parser& parser, std::initializer_list<const char*> lines) {
    uint32_t line_number = 1;
    for (const char* line: lines) {
        if (!parser.FeedLine(line, line_number++)) {
            return false;
        }
    }
    return parser.Finish();
}

MappedInstrument ParseAndMap(std::initializer_list<const char*> lines,
                             const char* path = "0:/wavex/sfz/test/test.sfz") {
    Parser parser;
    parser.Reset();
    EXPECT_TRUE(Feed(parser, lines));
    MappedInstrument mapped;
    Status status;
    EXPECT_TRUE(MapDocument(parser.GetDocument(), path, mapped, status));
    return mapped;
}

}  // namespace

TEST(SfzParserTest, ParsesBasicRegionAndSkipsUnknownOpcodes) {
    Parser parser;
    parser.Reset();
    ASSERT_TRUE(Feed(parser,
                     {"<region> lokey=48 hikey=72 lovel=10 hivel=100 mystery=ignored // c",
                      "sample=Middle C.wav"}));

    const Document& document = parser.GetDocument();
    ASSERT_EQ(document.total_regions, 1);
    ASSERT_EQ(document.stored_regions, 1);
    EXPECT_EQ(document.unknown_opcodes, 1);
    EXPECT_EQ(document.regions[0].lokey.value, 48);
    EXPECT_EQ(document.regions[0].hikey.value, 72);
    EXPECT_STREQ(document.regions[0].sample, "Middle C.wav");
}

TEST(SfzParserTest, CascadesGlobalAndGroupDefaultsIntoRegions) {
    Parser parser;
    parser.Reset();
    ASSERT_TRUE(Feed(parser,
                     {"<control> default_path=Samples/",
                      "<global> ampeg_release=0.25 cutoff=12000",
                      "<group> lovel=1 hivel=63",
                      "<region> key=c4 sample=soft.wav",
                      "<region> key=d4 sample=soft-d.wav",
                      "<group> lovel=64 hivel=127",
                      "<region> key=c4 sample=hard.wav"}));

    const Document& document = parser.GetDocument();
    ASSERT_EQ(document.stored_regions, 3);
    EXPECT_FLOAT_EQ(document.regions[0].release.value, 0.25f);
    EXPECT_FLOAT_EQ(document.regions[2].cutoff.value, 12000.0f);
    EXPECT_EQ(document.regions[0].lovel.value, 1);
    EXPECT_EQ(document.regions[1].hivel.value, 63);
    EXPECT_EQ(document.regions[2].lovel.value, 64);
    EXPECT_STREQ(document.regions[2].default_path, "Samples/");
}

TEST(SfzParserTest, ExpandsCommonDefineMacros) {
    Parser parser;
    parser.Reset();
    ASSERT_TRUE(Feed(parser,
                     {"#define $LOW 36",
                      "#define $HIGH 72",
                      "<region> lokey=$LOW hikey=$HIGH",
                      "sample=piano.wav"}));
    ASSERT_EQ(parser.GetDocument().stored_regions, 1);
    EXPECT_EQ(parser.GetDocument().regions[0].lokey.value, 36);
    EXPECT_EQ(parser.GetDocument().regions[0].hikey.value, 72);
}

TEST(SfzParserTest, ExpandsMacrosEmbeddedInSamplePaths) {
    Parser parser;
    parser.Reset();
    ASSERT_TRUE(Feed(parser,
                     {"#define $DIR Samples/Piano",
                      "#define $NAME Middle C",
                      "<region> sample=$DIR/$NAME.wav"}));
    ASSERT_EQ(parser.GetDocument().stored_regions, 1);
    EXPECT_STREQ(parser.GetDocument().regions[0].sample, "Samples/Piano/Middle C.wav");
}

TEST(SfzParserTest, IgnoredHeadersDoNotLeakThePreviousGroupDefaults) {
    Parser parser;
    parser.Reset();
    ASSERT_TRUE(Feed(parser,
                     {"<group> lovel=64",
                      "<region> sample=group.wav",
                      "<master>",
                      "<region> sample=global.wav"}));
    ASSERT_EQ(parser.GetDocument().stored_regions, 2);
    EXPECT_TRUE(parser.GetDocument().regions[0].lovel.present);
    EXPECT_FALSE(parser.GetDocument().regions[1].lovel.present);
}

TEST(SfzParserTest, RejectsInvalidRecognizedValues) {
    Parser parser;
    parser.Reset();
    EXPECT_FALSE(parser.FeedLine("<region> lovel=not-a-number sample=x.wav", 7));
    EXPECT_EQ(parser.GetStatus().error, Error::InvalidValue);
    EXPECT_EQ(parser.GetStatus().line, 7u);
}

TEST(SfzParserTest, RejectsMoreThanThirtyTwoRegionsWithExactCount) {
    Parser parser;
    parser.Reset();
    char line[64];
    for (int i = 0; i < 33; ++i) {
        std::snprintf(line, sizeof(line), "<region> key=%d sample=x.wav", i);
        ASSERT_TRUE(parser.FeedLine(line, static_cast<uint32_t>(i + 1)));
    }
    EXPECT_FALSE(parser.Finish());
    EXPECT_EQ(parser.GetStatus().error, Error::TooManyRegions);
    EXPECT_EQ(parser.GetStatus().region_count, 33);
}

TEST(SfzMapperTest, NoteNamesUseSfzC4EqualsMidi60Convention) {
    int32_t note = 0;
    EXPECT_TRUE(WaveX::AudioEngine::Sfz::detail::ParseKey("c4", note));
    EXPECT_EQ(note, 60);
    EXPECT_TRUE(WaveX::AudioEngine::Sfz::detail::ParseKey("c#4", note));
    EXPECT_EQ(note, 61);
    EXPECT_TRUE(WaveX::AudioEngine::Sfz::detail::ParseKey("db4", note));
    EXPECT_EQ(note, 61);
    EXPECT_TRUE(WaveX::AudioEngine::Sfz::detail::ParseKey("b3", note));
    EXPECT_EQ(note, 59);
    EXPECT_TRUE(WaveX::AudioEngine::Sfz::detail::ParseKey("127", note));
    EXPECT_EQ(note, 127);
    EXPECT_FALSE(WaveX::AudioEngine::Sfz::detail::ParseKey("c2147483647", note));
}

TEST(SfzMapperTest, MapsSupportedOpcodesAndUnitConversions) {
    const MappedInstrument mapped =
        ParseAndMap({"<region> key=c4 lovel=20 hivel=110 pitch_keycenter=f3 transpose=12 tune=-25",
                     "volume=-6.0206 pan=-100 offset=12 end=1000 loop_start=100 loop_end=900",
                     "loop_mode=loop_sustain group=3 off_by=3 cutoff=8000 ampeg_attack=0.01",
                     "ampeg_decay=0.2 ampeg_sustain=75 ampeg_release=0.4 sample=tone.wav"});

    ASSERT_EQ(mapped.zone_count, 1);
    const Zone& zone = mapped.instrument.zones[0];
    EXPECT_EQ(zone.key_lo, 60);
    EXPECT_EQ(zone.key_hi, 60);
    EXPECT_EQ(zone.vel_lo, 20);
    EXPECT_EQ(zone.vel_hi, 110);
    EXPECT_EQ(zone.root_note, 53);  // f3
    EXPECT_EQ(zone.coarse_tune, 12);
    EXPECT_EQ(zone.fine_tune, -25);
    EXPECT_NEAR(zone.gain, 0.5f, 1e-4f);
    EXPECT_FLOAT_EQ(zone.pan, 0.0f);
    EXPECT_EQ(zone.start_frame, 12u);
    EXPECT_EQ(zone.end_frame, 1000u);
    EXPECT_EQ(zone.loop_start, 100u);
    EXPECT_EQ(zone.loop_end, 900u);
    EXPECT_EQ(zone.loop_mode, 1);
    EXPECT_EQ(zone.choke_group, 3);
    EXPECT_FLOAT_EQ(zone.cutoff_hz, 8000.0f);
    EXPECT_FLOAT_EQ(zone.attack_s, 0.01f);
    EXPECT_FLOAT_EQ(zone.decay_s, 0.2f);
    EXPECT_FLOAT_EQ(zone.sustain, 0.75f);
    EXPECT_FLOAT_EQ(zone.release_s, 0.4f);
    EXPECT_TRUE(zone.in_use);
}

TEST(SfzMapperTest, PreservesZoneDefaultsWhenOpcodesAreMissing) {
    const MappedInstrument mapped = ParseAndMap({"<region> sample=plain.wav"});
    ASSERT_EQ(mapped.zone_count, 1);
    const Zone& zone = mapped.instrument.zones[0];
    EXPECT_EQ(zone.key_lo, 0);
    EXPECT_EQ(zone.key_hi, 127);
    EXPECT_EQ(zone.vel_lo, 1);
    EXPECT_EQ(zone.vel_hi, 127);
    EXPECT_EQ(zone.root_note, 60);
    EXPECT_FLOAT_EQ(zone.gain, 1.0f);
    EXPECT_FLOAT_EQ(zone.cutoff_hz, 20000.0f);
}

TEST(SfzMapperTest, KeySetsRangeAndRootWhenPitchCenterIsAbsent) {
    const MappedInstrument mapped = ParseAndMap({"<region> key=g4 sample=g.wav"});
    ASSERT_EQ(mapped.zone_count, 1);
    EXPECT_EQ(mapped.instrument.zones[0].key_lo, 67);
    EXPECT_EQ(mapped.instrument.zones[0].key_hi, 67);
    EXPECT_EQ(mapped.instrument.zones[0].root_note, 67);
}

TEST(SfzMapperTest, EndMinusOneDisablesRegion) {
    const MappedInstrument mapped =
        ParseAndMap({"<region> end=-1 sample=disabled.wav", "<region> sample=enabled.wav"});
    ASSERT_EQ(mapped.zone_count, 1);
    EXPECT_STREQ(mapped.sample_paths[0], "0:/wavex/sfz/test/enabled.wav");
}

TEST(SfzMapperTest, ResolvesDefaultPathAndNormalizesBackslashes) {
    const MappedInstrument mapped =
        ParseAndMap({"<control> default_path=Samples\\Piano", "<region> sample=Middle C.wav"},
                    "0:/wavex/sfz/grand/grand.sfz");
    ASSERT_EQ(mapped.zone_count, 1);
    EXPECT_STREQ(mapped.sample_paths[0], "0:/wavex/sfz/grand/Samples/Piano/Middle C.wav");
}

TEST(SfzMapperTest, RejectsInvertedRangesAfterClamping) {
    Parser parser;
    parser.Reset();
    ASSERT_TRUE(Feed(parser, {"<region> lokey=90 hikey=20 sample=bad.wav"}));
    MappedInstrument mapped;
    Status status;
    EXPECT_FALSE(MapDocument(parser.GetDocument(), "0:/bad.sfz", mapped, status));
    EXPECT_EQ(status.error, Error::InvalidRange);
}

TEST(SfzMapperTest, ClampsExtremeGainAndEnvelopeValuesToFiniteBounds) {
    const MappedInstrument mapped = ParseAndMap(
        {"<region> volume=100000 ampeg_attack=100000 ampeg_decay=-2 ampeg_release=100000 "
         "sample=safe.wav"});
    ASSERT_EQ(mapped.zone_count, 1);
    const Zone& zone = mapped.instrument.zones[0];
    EXPECT_TRUE(std::isfinite(zone.gain));
    EXPECT_NEAR(zone.gain, std::pow(10.0f, 24.0f / 20.0f), 1e-4f);
    EXPECT_FLOAT_EQ(zone.attack_s, 60.0f);
    EXPECT_FLOAT_EQ(zone.decay_s, 0.0f);
    EXPECT_FLOAT_EQ(zone.release_s, 60.0f);
}

TEST(SfzMapperTest, OneShotSetsZoneFlagWithoutLooping) {
    const MappedInstrument mapped = ParseAndMap({"<region> loop_mode=one_shot sample=hit.wav"});
    ASSERT_EQ(mapped.zone_count, 1);
    EXPECT_EQ(mapped.instrument.zones[0].loop_mode, 0);
    EXPECT_NE(mapped.instrument.zones[0].flags & ZONE_FLAG_ONE_SHOT, 0);
}

TEST(SfzSamplePlanTest, DeduplicatesPathsAndAssignsStableIds) {
    MappedInstrument mapped = ParseAndMap({"<region> key=60 sample=shared.wav",
                                           "<region> key=61 transpose=1 sample=shared.wav",
                                           "<region> key=62 sample=other.wav"});
    SamplePlan plan;
    Status status;
    ASSERT_TRUE(BuildSamplePlan(mapped, plan, status));
    ASSERT_EQ(plan.count, 2);
    EXPECT_EQ(mapped.instrument.zones[0].sample_id, 1);
    EXPECT_EQ(mapped.instrument.zones[1].sample_id, 1);
    EXPECT_EQ(mapped.instrument.zones[2].sample_id, 2);
}

TEST(SfzSamplePlanTest, SampleTableBacksInstrumentResolver) {
    std::array<int16_t, 4> first{{1, 2, 3, 4}};
    std::array<int16_t, 6> second{{1, 2, 3, 4, 5, 6}};
    SampleTable table;
    ASSERT_TRUE(table.Bind(1, SampleRef{first.data(), 4, 1, 44100}));
    ASSERT_TRUE(table.Bind(2, SampleRef{second.data(), 3, 2, 48000}));

    const SampleResolver resolver = table.Resolver();
    EXPECT_EQ(resolver.Get(1).data, first.data());
    EXPECT_EQ(resolver.Get(2).channels, 2);
    EXPECT_FALSE(resolver.Get(99).valid());
}

TEST(SfzSamplePlanTest, BudgetGuardReservesRamAndCapsEachSample) {
    const SampleProbe probes[] = {{1024}, {2048}};
    uint32_t total = 0;
    Status status;
    EXPECT_TRUE(ValidateSampleBudget(probes, 2, 16 * 1024, 8 * 1024, 4096, total, status));
    EXPECT_EQ(total, 3072u);

    status = Status{};
    EXPECT_FALSE(ValidateSampleBudget(probes, 2, 10 * 1024, 8 * 1024, 4096, total, status));
    EXPECT_EQ(status.error, Error::BudgetExceeded);

    const SampleProbe oversized[] = {{4097}};
    status = Status{};
    EXPECT_FALSE(ValidateSampleBudget(oversized, 1, 64 * 1024, 0, 4096, total, status));
    EXPECT_EQ(status.error, Error::SampleTooLarge);
}
