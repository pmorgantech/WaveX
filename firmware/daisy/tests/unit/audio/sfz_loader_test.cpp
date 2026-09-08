#include "audio/sfz_loader.hpp"

#include <gtest/gtest.h>

#include "comm/daisy_uart_link.h"
#include "fatfs_mock.h"

#include <array>
#include <cstring>
#include <vector>

namespace WaveX::Comm {
static WaveX::Protocol::InstZoneSyncMessage last_pad_map;
int UartLinkSend(uint16_t type, const void* payload, uint16_t length) {
    if (type == WaveX::Protocol::MSG_INST_ZONE_SYNC && length == sizeof(last_pad_map))
        std::memcpy(&last_pad_map, payload, length);
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
        auto* record = pool_.FindByPath(path);
        return record ? record->sample_id : 0;
    }
    uint32_t next_request_ = 100;
    InstZoneSyncMessage Edit(uint8_t op,
                             uint8_t track,
                             const char* name = "",
                             uint8_t pad = 0,
                             uint16_t sample = 0,
                             uint8_t choke = 0) {
        InstOpMessage request(++next_request_, track, op, name);
        request.pad_index = pad;
        request.pad_sample_id = sample;
        request.pad_choke = choke;
        SfzLoader::Begin(request);
        for (int i = 0; i < 1000 && SfzLoader::Busy(); ++i) {
            if (SfzLoader::VoiceStopTrack() != 0xFF)
                SfzLoader::ConfirmVoicesStopped(pool_, memory_);
            else
                SfzLoader::Pump(pool_, memory_, io_.data(), io_.size());
        }
        SfzLoader::PumpEditorReply();
        return WaveX::Comm::last_pad_map;
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

TEST_F(SfzLoaderTest, DistinctCollidingPathsLoadIndependentPcmAllocations) {
    const char first[] = "0:/samples/d458dfeb4949ec65.wav";
    const char second[] = "0:/samples/06da2fcf0c771773.wav";
    ASSERT_EQ(WaveX::Audio::HashSamplePath(first), WaveX::Audio::HashSamplePath(second));
    const char sfz[] =
        "<region> sample=d458dfeb4949ec65.wav key=60\n"
        "<region> sample=06da2fcf0c771773.wav key=61\n";
    MockFatFS::Instance().AddFile("0:/samples/collision.sfz", {sfz, sfz + std::strlen(sfz)});
    auto first_pcm = PcmWave();
    auto second_pcm = PcmWave();
    second_pcm[44] = 99;
    MockFatFS::Instance().AddFile(first, first_pcm);
    MockFatFS::Instance().AddFile(second, second_pcm);
    ASSERT_TRUE(SfzLoader::Load("0:/samples/collision.sfz",
                                0,
                                pool_,
                                memory_,
                                io_.data(),
                                static_cast<uint32_t>(io_.size())));
    ASSERT_EQ(pool_.Count(), 2u);
    const auto* a = pool_.FindByPath(first);
    const auto* b = pool_.FindByPath(second);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a->sample_id, b->sample_id);
    void* a_pcm = nullptr;
    void* b_pcm = nullptr;
    ASSERT_TRUE(memory_.ptr(a->payload.handle, &a_pcm));
    ASSERT_TRUE(memory_.ptr(b->payload.handle, &b_pcm));
    EXPECT_NE(a_pcm, b_pcm);
    EXPECT_EQ(static_cast<int16_t*>(a_pcm)[0], 1);
    EXPECT_EQ(static_cast<int16_t*>(b_pcm)[0], 99);
}

TEST_F(SfzLoaderTest, PublishedModulationCannotMutateTheCallbacksCurrentTable) {
    const ModSlot* callback_table = SfzLoader::GetModSlots(0);
    ASSERT_NE(callback_table, nullptr);
    ModSlot edited;
    edited.source = SRC_LFO1;
    edited.dest = DEST_CUTOFF;
    edited.depth = 16384;
    ASSERT_TRUE(SfzLoader::SetModSlot(0, 0, edited));
    EXPECT_EQ(callback_table[0].depth, 0);
    EXPECT_EQ(SfzLoader::GetModSlots(0)[0].depth, edited.depth);
}

TEST_F(SfzLoaderTest, ModulationEditsToDifferentTracksDoNotCoalesceAway) {
    ModSlot first;
    first.depth = 1234;
    ModSlot second;
    second.depth = -5678;
    ASSERT_TRUE(SfzLoader::SetModSlot(1, 0, first));
    ASSERT_TRUE(SfzLoader::SetModSlot(2, 0, second));
    EXPECT_EQ(SfzLoader::GetModSlots(1)[0].depth, first.depth);
    EXPECT_EQ(SfzLoader::GetModSlots(2)[0].depth, second.depth);
}

TEST_F(SfzLoaderTest, RebindingIsRefusedWhileImportOwnsThePoolTransaction) {
    ASSERT_TRUE(Load(0));
    const uint16_t sample = SampleId("/kits/a.wav");
    ASSERT_TRUE(SfzLoader::Begin(InstOpMessage(1, 0, INST_OP_SFZ_LOAD, "/kits/kit.sfz")));
    EXPECT_FALSE(SfzLoader::BindSample(pool_, memory_, 0, sample));
    EXPECT_FALSE(SfzLoader::BindSample(pool_, memory_, 1, sample));
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 1);
    EXPECT_EQ(pool_.Count(), 2u);
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

TEST_F(SfzLoaderTest, KitPadReferencesAreSharedAcrossPadsAndTracks) {
    ASSERT_TRUE(Load(0));
    ASSERT_TRUE(Load(1));
    const auto a = SampleId("/kits/a.wav"), b = SampleId("/kits/b.wav");
    EXPECT_EQ(Edit(INST_OP_NEW, 0, "Kit").error, INST_ERROR_NONE);
    EXPECT_EQ(pool_.Find(a)->used_by, 2);
    EXPECT_TRUE(SfzLoader::TrackLoaded(0));
    EXPECT_EQ(Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 0, a, 3).error, INST_ERROR_NONE);
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 15, a, 7);
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 0, b, 2);
    EXPECT_EQ(pool_.Find(a)->used_by, 3);  // pad 16 still holds it
    auto map = Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 15, 0);
    EXPECT_EQ(pool_.Find(a)->used_by, 2);  // other Track remains
    EXPECT_EQ(map.pads[0].sample_id, b);
    EXPECT_EQ(map.pads[0].choke_group, 2);
    EXPECT_EQ(map.pads[15].sample_id, 0);
    EXPECT_EQ(SfzLoader::BoundSample(0), 0);  // a kit is an Instrument binding
    EXPECT_EQ(Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 16, a).error, INST_ERROR_BAD_FILE);
    EXPECT_EQ(Edit(INST_OP_SET_PAD_SAMPLE, 1, "", 0, a).error, INST_ERROR_BAD_FILE);
}
TEST_F(SfzLoaderTest, SparseKitSaveReloadPreservesPadAndCardIdentity) {
    ASSERT_TRUE(Load(1));
    const auto a = SampleId("/kits/a.wav");
    Edit(INST_OP_NEW, 0, "Sparse");
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 15, a, 7);
    auto saved = Edit(INST_OP_SAVE, 0, "Sparse saved");
    ASSERT_EQ(saved.error, INST_ERROR_NONE);
    ASSERT_NE(MockFatFS::Instance().GetFile("0:/wavex/instruments/Sparse saved.wxi"), nullptr);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 1, 0));
    ASSERT_EQ(pool_.Count(), 0u);
    ASSERT_TRUE(SfzLoader::Load(
        "0:/wavex/instruments/Sparse saved.wxi", 2, pool_, memory_, io_.data(), io_.size()));
    auto loaded = Edit(INST_OP_GET_PAD_MAP, 2);
    EXPECT_EQ(loaded.editable, 1);
    EXPECT_STREQ(loaded.name, "Sparse saved");
    EXPECT_EQ(loaded.pads[0].sample_id, 0);
    const auto reloaded = SampleId("/kits/a.wav");
    EXPECT_NE(reloaded, a);
    EXPECT_EQ(loaded.pads[15].sample_id, reloaded);
    EXPECT_EQ(loaded.pads[15].choke_group, 7);
    EXPECT_EQ(loaded.pads[15].note, 75);
    EXPECT_EQ(pool_.Find(reloaded)->used_by, 4);
}
TEST_F(SfzLoaderTest, FailedSaveNeverReplacesAnExistingCopy) {
    Edit(INST_OP_NEW, 0, "Empty");
    ASSERT_EQ(Edit(INST_OP_SAVE, 0, "Saved").error, INST_ERROR_NONE);
    auto& fs = MockFatFS::Instance();
    const auto original = *fs.GetFile("0:/wavex/instruments/Saved.wxi");
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Saved").error, INST_ERROR_EXISTS);
    EXPECT_EQ(*fs.GetFile("0:/wavex/instruments/Saved.wxi"), original);
    fs.write_limit = 31;
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Short").error, INST_ERROR_IO);
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Short.wxi"), nullptr);
    fs.write_limit = -1;
    fs.close_result = FR_DISK_ERR;
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Close").error, INST_ERROR_IO);
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Close.wxi"), nullptr);
    fs.close_result = FR_OK;
    fs.rename_result = FR_DISK_ERR;
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Rename").error, INST_ERROR_IO);
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Rename.wxi"), nullptr);
    EXPECT_EQ(*fs.GetFile("0:/wavex/instruments/Saved.wxi"), original);
}
TEST_F(SfzLoaderTest, EmptyKitReloadAndLostReplyRecovery) {
    Edit(INST_OP_NEW, 0, "Empty");
    auto saved = Edit(INST_OP_SAVE, 0, "Empty");
    ASSERT_EQ(saved.error, INST_ERROR_NONE);
    auto read = Edit(INST_OP_GET_PAD_MAP, 0);
    EXPECT_EQ(read.completed_request_id, saved.completed_request_id);
    EXPECT_EQ(read.error, saved.error);
    ASSERT_TRUE(SfzLoader::Load(
        "0:/wavex/instruments/Empty.wxi", 1, pool_, memory_, io_.data(), io_.size()));
    read = Edit(INST_OP_GET_PAD_MAP, 1);
    EXPECT_EQ(read.loaded, 1);
    EXPECT_EQ(read.editable, 1);
    EXPECT_EQ(read.pads[15].sample_id, 0);
}

TEST_F(SfzLoaderTest, KitReplacementGatesNewTriggersUntilStopAcknowledgement) {
    ASSERT_TRUE(Load(0));
    ASSERT_TRUE(Load(1));
    const auto old = SampleId("/kits/a.wav");
    ASSERT_TRUE(SfzLoader::Begin({900, 0, INST_OP_NEW, "New kit"}));
    EXPECT_TRUE(SfzLoader::TrackLoading(0));
    EXPECT_FALSE(SfzLoader::TrackLoading(1));
    EXPECT_EQ(SfzLoader::VoiceStopTrack(), 0);
    EXPECT_EQ(pool_.Find(old)->used_by, 3);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    EXPECT_FALSE(SfzLoader::TrackLoading(0));
    EXPECT_EQ(pool_.Find(old)->used_by, 2);
}
}  // namespace
