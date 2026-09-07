#include "audio/sfz_loader.hpp"

#include <gtest/gtest.h>

#include "comm/daisy_uart_link.h"
#include "fatfs_mock.h"

#include <array>
#include <cstring>
#include <vector>

namespace WaveX::Comm {
int UartLinkSend(uint16_t, const void*, uint16_t length) {
    return length;
}
}  // namespace WaveX::Comm

namespace {
using namespace WaveX::AudioEngine;
using namespace WaveX::Protocol;

// Loader admission includes the production 8 MiB reserve. Keep real limits
// rather than weakening them for the tests.
alignas(32) std::array<uint8_t, 12 * 1024 * 1024> arena;
std::array<SamplePool::Record, WAVEX_SAMPLE_POOL_CAPACITY> records;

std::vector<uint8_t> PcmWave() {
    const uint8_t header[] = {'R', 'I', 'F', 'F',  44, 0, 0, 0, 'W', 'A', 'V', 'E',  'f',
                              'm', 't', ' ', 16,   0,  0, 0, 1, 0,   1,   0,   0x80, 0xbb,
                              0,   0,   0,   0x77, 1,  0, 2, 0, 16,  0,   'd', 'a',  't',
                              'a', 8,   0,   0,    0,  1, 0, 2, 0,   3,   0,   4,    0};
    return {std::begin(header), std::end(header)};
}

class SfzLoaderTest : public ::testing::Test {
   protected:
    SfzLoaderTest() : pool_(records.data()) {}
    void SetUp() override {
        MockFatFS::Instance().Reset();
        SfzLoader::Reset();
        ASSERT_TRUE(memory_.init(arena.data(), static_cast<uint32_t>(arena.size()), 256 * 1024));
        const char sfz[] = "<region> sample=a.wav key=60\n<region> sample=b.wav key=61\n";
        MockFatFS::Instance().AddFile("/kits/kit.sfz", {sfz, sfz + std::strlen(sfz)});
        MockFatFS::Instance().AddFile("/kits/a.wav", PcmWave());
        MockFatFS::Instance().AddFile("/kits/b.wav", PcmWave());
    }
    void TearDown() override {
        SfzLoader::Reset();
        MockFatFS::Instance().Reset();
    }
    bool Load(uint8_t track) {
        return SfzLoader::Load(
            "/kits/kit.sfz", track, pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size()));
    }
    uint16_t SampleId(const char* path) {
        auto* record = pool_.FindByPath(WaveX::Audio::HashSamplePath(path));
        return record ? record->sample_id : 0;
    }
    SamplePool pool_;
    SampleMemMgr memory_;
    std::array<uint8_t, 512> io_{};
};

TEST_F(SfzLoaderTest, AssigningOwnImportedSampleTransfersItsReference) {
    ASSERT_TRUE(Load(0));
    const uint16_t selected = SampleId("/kits/a.wav");
    const uint16_t discarded = SampleId("/kits/b.wav");
    ASSERT_NE(selected, 0);
    ASSERT_NE(discarded, 0);
    void* before = nullptr;
    ASSERT_TRUE(memory_.ptr(pool_.Find(selected)->payload.handle, &before));

    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, selected));
    const auto* retained = pool_.Find(selected);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->used_by, 1);
    EXPECT_FALSE(retained->pinned);
    EXPECT_EQ(SfzLoader::BoundSample(0), selected);
    EXPECT_EQ(pool_.Find(discarded), nullptr);
    void* after = nullptr;
    ASSERT_TRUE(memory_.ptr(retained->payload.handle, &after));
    EXPECT_EQ(after, before);
    EXPECT_EQ(static_cast<int16_t*>(after)[3], 4);
    wxsamp_stats_t stats{};
    memory_.stats(&stats);
    EXPECT_EQ(stats.objects_alive, 1u);

    // Repeated Assign also retains it, and a real unbind then releases it.
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, selected));
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    EXPECT_EQ(pool_.Count(), 0u);
    memory_.stats(&stats);
    EXPECT_EQ(stats.objects_alive, 0u);
}

TEST_F(SfzLoaderTest, SharedImportRemainsResidentUntilLastTrackReleasesIt) {
    ASSERT_TRUE(Load(0));
    const uint16_t sample = SampleId("/kits/a.wav");
    ASSERT_TRUE(Load(1));
    EXPECT_EQ(pool_.Count(), 2u);
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 3);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 2);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 1, 0));
    EXPECT_EQ(pool_.Count(), 0u);
}

TEST_F(SfzLoaderTest, RuntimeReplacementWaitsForExplicitStopAcknowledgement) {
    ASSERT_TRUE(Load(0));
    const uint16_t original = SampleId("/kits/a.wav");
    ASSERT_TRUE(SfzLoader::Begin(InstOpMessage(1, 0, INST_OP_SFZ_LOAD, "/kits/kit.sfz")));
    for (int pass = 0; pass < 100 && SfzLoader::VoiceStopTrack() == 0xFF; ++pass) {
        SfzLoader::Pump(pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size()));
    }
    ASSERT_EQ(SfzLoader::VoiceStopTrack(), 0);
    for (int pass = 0; pass < 100; ++pass) {
        SfzLoader::Pump(pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size()));
    }
    EXPECT_NE(pool_.Find(original), nullptr);
    EXPECT_TRUE(SfzLoader::Busy());
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    EXPECT_EQ(pool_.Find(original), nullptr);
    for (int pass = 0; pass < 100 && SfzLoader::Busy(); ++pass) {
        SfzLoader::Pump(pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size()));
    }
    EXPECT_FALSE(SfzLoader::Busy());
    EXPECT_TRUE(SfzLoader::TrackLoaded(0));
}

TEST_F(SfzLoaderTest, MissingSampleProbeLeavesOriginalTrackAndMemoryIntact) {
    ASSERT_TRUE(Load(0));
    const uint16_t original = SampleId("/kits/a.wav");
    const char invalid[] = "<region> sample=absent.wav key=60\n";
    MockFatFS::Instance().AddFile("/kits/bad.sfz", {invalid, invalid + std::strlen(invalid)});
    EXPECT_FALSE(SfzLoader::Load(
        "/kits/bad.sfz", 0, pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size())));
    EXPECT_TRUE(SfzLoader::TrackLoaded(0));
    EXPECT_NE(pool_.Find(original), nullptr);
    EXPECT_EQ(pool_.Count(), 2u);
}
}  // namespace
