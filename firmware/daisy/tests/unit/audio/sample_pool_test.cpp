#include "audio/sample_pool.hpp"

#include <gtest/gtest.h>

#include "sdram_layout.h"

#include <cstring>
#include <string>

using namespace WaveX::AudioEngine;

namespace {

ResidentSampleInfo MakeResident() {
    ResidentSampleInfo r{};
    r.data_size = 4096;
    r.sample_rate = 44100;
    r.channels = 1;
    r.bit_depth = 16;
    r.total_frames = 2048;
    return r;
}

LoadedSampleInfo Fill(const char* path) {
    LoadedSampleInfo info{};
    wxsamp_t handle{};
    handle.len = 4096;
    FillLoadedSample(info, 7, path, MakeResident(), handle);
    return info;
}

}  // namespace

// A Pool id names a slot in this boot's registry and means nothing in a
// file, so saving an Instrument needs the sample's card path back. That is
// what `path` is for, and why it cannot be `meta.name`.
TEST(SamplePoolTest, ShortPathIsKeptInBothPlaces) {
    const char* kPath = "/samples/kick.wav";
    const LoadedSampleInfo info = Fill(kPath);
    EXPECT_STREQ(info.path, kPath);
    EXPECT_STREQ(info.meta.name, kPath);
}

// The case that made this field necessary: a real card path from the bench
// is longer than the wire's display field, so meta.name truncates it into
// something that opens nothing - while `path` keeps it whole.
TEST(SamplePoolTest, RealCardPathSurvivesEvenThoughMetaNameTruncatesIt) {
    const char* kPath = "/99 - Vintage Sound Library/Minimoog/Samples/SawSynBass-C2.wav";
    ASSERT_GT(std::strlen(kPath), sizeof(WaveX::Protocol::SampleMetadata::name) - 1)
        << "this test is pointless unless the path really does overflow meta.name";
    ASSERT_LT(std::strlen(kPath), sizeof(LoadedSampleInfo::path));

    const LoadedSampleInfo info = Fill(kPath);
    EXPECT_STREQ(info.path, kPath) << "the authoritative path must be whole";
    EXPECT_STRNE(info.meta.name, kPath) << "meta.name is the display field and does truncate";
}

// A path at exactly the bound still fits (the field holds bound-1 characters
// plus a terminator).
TEST(SamplePoolTest, PathAtTheBoundIsKept) {
    std::string path(sizeof(LoadedSampleInfo::path) - 1, 'a');
    path[0] = '/';
    const LoadedSampleInfo info = Fill(path.c_str());
    EXPECT_STREQ(info.path, path.c_str());
}

// An SFZ region can resolve a path longer than this bound. A truncated path
// is not a shorter path, it is a wrong one - so none is stored, and the
// save that needs it fails loudly rather than writing a zone that silently
// never loads.
TEST(SamplePoolTest, OverlongPathIsRefusedRatherThanTruncated) {
    std::string path(sizeof(LoadedSampleInfo::path) + 20, 'b');
    path[0] = '/';
    const LoadedSampleInfo info = Fill(path.c_str());
    EXPECT_STREQ(info.path, "") << "a truncated path would open the wrong file, or none";
    // The rest of the record is still valid - the sample plays, it just
    // cannot be named in a file.
    EXPECT_EQ(info.sample_id, 7);
    EXPECT_EQ(info.meta.total_frames, 2048u);
}

TEST(SamplePoolTest, NullPathLeavesTheFieldEmpty) {
    const LoadedSampleInfo info = Fill(nullptr);
    EXPECT_STREQ(info.path, "");
}

// FillLoadedSample resets the record: a slot reused for another file must
// not keep the previous sample's path.
TEST(SamplePoolTest, RefillClearsThePreviousPath) {
    LoadedSampleInfo info = Fill("/samples/first.wav");
    ASSERT_STREQ(info.path, "/samples/first.wav");

    wxsamp_t handle{};
    handle.len = 4096;
    FillLoadedSample(info, 9, "/b.wav", MakeResident(), handle);
    EXPECT_STREQ(info.path, "/b.wav");
    EXPECT_EQ(info.sample_id, 9);
}

// The records must still fit their SDRAM partition now that each carries a
// path. audio_engine.cpp static_asserts this too, but that only builds for
// the target; this runs on host, where a regression is seen sooner.
TEST(SamplePoolTest, RecordsFitTheirPartition) {
    constexpr size_t kBytes = sizeof(SamplePool::Record) * WAVEX_SAMPLE_POOL_CAPACITY;
    EXPECT_LE(kBytes, WaveX::SdramLayout::kSampleRegistryBytes)
        << sizeof(SamplePool::Record) << " B x " << WAVEX_SAMPLE_POOL_CAPACITY << " records";
}
