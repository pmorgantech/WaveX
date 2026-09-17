#include "audio/sfz_loader.hpp"

#include <gtest/gtest.h>

#include "comm/daisy_uart_link.h"
#include "fatfs_mock.h"

#include "audio/sample_pool_stage.hpp"
#include "wxi/wxi.hpp"
#include <array>
#include <cstring>
#include <vector>

namespace WaveX::Comm {
static WaveX::Protocol::InstKeyMapSyncMessage last_key_map;
static WaveX::Protocol::InstOscSyncMessage last_osc;
static WaveX::Protocol::TrackStateMessage last_track_state;
static WaveX::Protocol::InstZoneSyncMessage last_pad_map;
static WaveX::Protocol::InstPadSoundSyncMessage last_pad_sound;
int UartLinkSend(uint16_t type, const void* payload, uint16_t length) {
    if (type == WaveX::Protocol::MSG_INST_OSC_SYNC && length == sizeof(last_osc))
        std::memcpy(&last_osc, payload, length);
    if (type == WaveX::Protocol::MSG_INST_KEY_MAP_SYNC && length == sizeof(last_key_map))
        std::memcpy(&last_key_map, payload, length);
    if (type == WaveX::Protocol::MSG_INST_ZONE_SYNC && length == sizeof(last_pad_map))
        std::memcpy(&last_pad_map, payload, length);
    if (type == WaveX::Protocol::MSG_INST_PAD_SOUND_SYNC && length == sizeof(last_pad_sound))
        std::memcpy(&last_pad_sound, payload, length);
    if (type == WaveX::Protocol::MSG_TRACK_STATE && length == sizeof(last_track_state))
        std::memcpy(&last_track_state, payload, length);
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

std::vector<uint8_t> PcmWave(uint32_t data_bytes = 8, uint16_t channels = 1) {
    const uint8_t header[] = {'R', 'I', 'F', 'F',  44, 0, 0, 0, 'W', 'A', 'V', 'E',  'f',
                              'm', 't', ' ', 16,   0,  0, 0, 1, 0,   1,   0,   0x80, 0xbb,
                              0,   0,   0,   0x77, 1,  0, 2, 0, 16,  0,   'd', 'a',  't',
                              'a', 8,   0,   0,    0,  1, 0, 2, 0,   3,   0,   4,    0};
    std::vector<uint8_t> wave(std::begin(header), std::end(header));
    wave.resize(44 + data_bytes);
    auto put32 = [&](size_t at, uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte)
            wave[at + byte] = static_cast<uint8_t>(value >> (8 * byte));
    };
    put32(4, 36 + data_bytes);
    put32(28, 48000u * channels * 2u);
    put32(40, data_bytes);
    wave[22] = static_cast<uint8_t>(channels);
    wave[32] = static_cast<uint8_t>(channels * 2);
    return wave;
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
        SfzLoader::SetLoadedSampleResolver({});
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
    // Model the direct WAV loader's resident result, which admits samples
    // against the arena rather than the smaller Instrument-import limit.
    void AddResidentWave(const char* path, uint32_t data_bytes, uint16_t channels = 1) {
        const auto wave = PcmWave(data_bytes, channels);
        MockFatFS::Instance().AddFile(path, wave);
        SamplePool::Record* record = nullptr;
        ASSERT_EQ(pool_.AdmitPath(path, &record), SamplePool::Admit::Ok);
        wxsamp_t handle{};
        ASSERT_TRUE(memory_.alloc(data_bytes, &handle));
        void* pcm = nullptr;
        ASSERT_TRUE(memory_.ptr(handle, &pcm));
        std::memcpy(pcm, wave.data() + 44, data_bytes);
        ResidentSampleInfo info{record->sample_id,
                                data_bytes,
                                48000,
                                data_bytes / (channels * 2u),
                                static_cast<uint8_t>(channels),
                                16};
        FillLoadedSample(record->payload, record->sample_id, path, info, handle);
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

TEST_F(SfzLoaderTest, ProjectSnapshotPreservesLiveNameUndoAndEditorRevision) {
    ASSERT_TRUE(Load(0));
    const auto before = SfzLoader::ReadEditState(0);
    InstEditOpMessage edit;
    edit.request_id = 7000;
    edit.revision = before.revision;
    edit.op = INST_EDIT_FILTER;
    edit.sound.cutoff_hz = 1234;
    ASSERT_TRUE(SfzLoader::OnEditOp(edit));
    const auto changed = SfzLoader::ReadEditState(0);
    ASSERT_TRUE(changed.dirty);
    const auto sample = SampleId("/kits/a.wav");
    const auto mask = pool_.Find(sample)->used_by;
    const char* path = "0:/wavex/projects/Session/track01.wxi";
    ASSERT_TRUE(SfzLoader::BeginProjectSnapshot(0, path));
    for (int i = 0; i < 1000 && SfzLoader::Busy(); ++i)
        SfzLoader::Pump(pool_, memory_, io_.data(), io_.size());
    ASSERT_FALSE(SfzLoader::Busy());
    EXPECT_EQ(SfzLoader::ProjectSnapshotError(), INST_ERROR_NONE);
    EXPECT_STREQ(SfzLoader::TrackName(0), "kit.sfz");
    EXPECT_EQ(SfzLoader::ReadEditState(0).revision, changed.revision);
    EXPECT_TRUE(SfzLoader::ReadEditState(0).dirty);
    EXPECT_EQ(pool_.Find(sample)->used_by, mask);
    ASSERT_TRUE(SfzLoader::Load(path, 1, pool_, memory_, io_.data(), io_.size()));
    EXPECT_FLOAT_EQ(SfzLoader::ReadEditState(1).sound.cutoff_hz, 1234);
    EXPECT_STREQ(SfzLoader::TrackName(1), "kit.sfz");
    edit.request_id++;
    edit.revision = changed.revision;
    edit.op = INST_EDIT_REVERT;
    EXPECT_TRUE(SfzLoader::OnEditOp(edit));
    EXPECT_FLOAT_EQ(SfzLoader::ReadEditState(0).sound.cutoff_hz, before.sound.cutoff_hz);
}

TEST_F(SfzLoaderTest, ProjectSnapshotRefusesFullCardAndUnadmittedDependencies) {
    ASSERT_TRUE(Load(0));
    auto& fs = MockFatFS::Instance();
    const char* path = "0:/wavex/projects/Session/track01.wxi";
    fs.free_clusters = 0;
    ASSERT_TRUE(SfzLoader::BeginProjectSnapshot(0, path));
    for (int i = 0; i < 1000 && SfzLoader::Busy(); ++i)
        SfzLoader::Pump(pool_, memory_, io_.data(), io_.size());
    EXPECT_EQ(SfzLoader::ProjectSnapshotError(), INST_ERROR_NO_SPACE);
    EXPECT_EQ(fs.GetFile(path), nullptr);
    fs.free_clusters = 1024 * 1024;
    fs.AddFile("/kits/a.wav", PcmWave(WAVEX_INST_MAX_RAM_SAMPLE_BYTES + 2));
    ASSERT_TRUE(SfzLoader::BeginProjectSnapshot(0, path));
    for (int i = 0; i < 1000 && SfzLoader::Busy(); ++i)
        SfzLoader::Pump(pool_, memory_, io_.data(), io_.size());
    EXPECT_EQ(SfzLoader::ProjectSnapshotError(), INST_ERROR_UNSUPPORTED_SAMPLE);
    EXPECT_EQ(fs.GetFile(path), nullptr);
    EXPECT_TRUE(SfzLoader::TrackLoaded(0));
    EXPECT_FALSE(SfzLoader::BeginProjectSnapshot(0, "0:/wavex/projects/../escape.wxi"));
}

TEST_F(SfzLoaderTest, SaveRejectsOversizedResidentDependencyAndPreservesTrackAndUndo) {
    AddResidentWave("/kits/large.wav", WAVEX_INST_MAX_RAM_SAMPLE_BYTES + 2);
    const auto sample = SampleId("/kits/large.wav");
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, sample));
    const auto original = pool_.Find(sample)->payload;
    const auto before = SfzLoader::ReadEditState(0);
    InstEditOpMessage op;
    op.request_id = 9000;
    op.revision = before.revision;
    op.op = INST_EDIT_FILTER;
    op.sound.cutoff_hz = 1234;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    const auto edited = SfzLoader::ReadEditState(0);

    const auto saved = Edit(INST_OP_SAVE, 0, "Too large");
    EXPECT_EQ(saved.error, INST_ERROR_UNSUPPORTED_SAMPLE);
    EXPECT_EQ(saved.completed_request_id, next_request_);
    EXPECT_EQ(MockFatFS::Instance().GetFile("0:/wavex/instruments/Too large.wxi"), nullptr);
    EXPECT_EQ(MockFatFS::Instance().GetFile("0:/wavex/instruments/.Too large-00000065.tmp"),
              nullptr);
    EXPECT_EQ(SfzLoader::BoundSample(0), sample);
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 1);
    EXPECT_EQ(pool_.Find(sample)->payload.handle.len, original.handle.len);
    EXPECT_EQ(pool_.Find(sample)->payload.loaded_bytes, original.loaded_bytes);
    wxsamp_stats_t stats{};
    memory_.stats(&stats);
    EXPECT_EQ(stats.objects_alive, 1u);
    auto state = SfzLoader::ReadEditState(0);
    EXPECT_EQ(state.revision, edited.revision);
    EXPECT_TRUE(state.dirty);
    EXPECT_FLOAT_EQ(state.sound.cutoff_hz, 1234);
    op.request_id++;
    op.revision = state.revision;
    op.op = INST_EDIT_REVERT;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    EXPECT_FLOAT_EQ(SfzLoader::ReadEditState(0).sound.cutoff_hz, before.sound.cutoff_hz);
}

TEST_F(SfzLoaderTest, SaveAtInstrumentLimitRecallsStereoAfterPoolRelease) {
    AddResidentWave("/kits/boundary.wav", WAVEX_INST_MAX_RAM_SAMPLE_BYTES, 2);
    const auto sample = SampleId("/kits/boundary.wav");
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, sample));
    ASSERT_EQ(Edit(INST_OP_SAVE, 0, "Boundary").error, INST_ERROR_NONE);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    ASSERT_EQ(pool_.Count(), 0u);
    ASSERT_TRUE(SfzLoader::Load("0:/wavex/instruments/Boundary.wxi",
                                0,
                                pool_,
                                memory_,
                                io_.data(),
                                static_cast<uint32_t>(io_.size())));
    const auto* record = pool_.FindByPath("/kits/boundary.wav");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->payload.loaded_bytes, WAVEX_INST_MAX_RAM_SAMPLE_BYTES);
    EXPECT_EQ(record->payload.channels, 2);
}

TEST_F(SfzLoaderTest, SaveChecksCardDependenciesEvenWhenPoolMetadataIsValid) {
    ASSERT_TRUE(Load(0));
    auto& fs = MockFatFS::Instance();
    ASSERT_TRUE(fs.RemoveFile("/kits/b.wav"));
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Missing").error, INST_ERROR_MISSING_SAMPLES);
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Missing.wxi"), nullptr);
    auto truncated = PcmWave();
    truncated.pop_back();
    fs.AddFile("/kits/b.wav", truncated);
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Truncated").error, INST_ERROR_UNSUPPORTED_SAMPLE);
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Truncated.wxi"), nullptr);
    EXPECT_EQ(pool_.Count(), 2u);
    EXPECT_TRUE(SfzLoader::TrackLoaded(0));
    fs.AddFile("/kits/b.wav", PcmWave());
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Recovered").error, INST_ERROR_NONE);
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

TEST_F(SfzLoaderTest, SaveChecksFreeSpaceBeforePublishingAndRetainsName) {
    ASSERT_EQ(Edit(INST_OP_NEW, 0, "Original").error, INST_ERROR_NONE);
    auto& fs = MockFatFS::Instance();
    fs.free_clusters = 0;
    auto reply = Edit(INST_OP_SAVE, 0, "Full");
    EXPECT_EQ(reply.error, INST_ERROR_NO_SPACE);
    EXPECT_STREQ(reply.name, "Original");
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Full.wxi"), nullptr);
    fs.free_result = FR_DISK_ERR;
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Unknown").error, INST_ERROR_IO);
    EXPECT_EQ(fs.GetFile("0:/wavex/instruments/Unknown.wxi"), nullptr);
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

TEST_F(SfzLoaderTest, PreparingOneTrackLeavesOtherPreparedTracksUntouched) {
    ASSERT_TRUE(Load(0));
    ASSERT_TRUE(Load(1));
    SfzLoader::SetLoadedSampleResolver({nullptr, [](const void*, uint16_t) {
                                            static const int16_t pcm[4]{};
                                            SampleRef ref;
                                            ref.data = pcm;
                                            ref.frames = 4;
                                            ref.sample_rate_hz = 48000;
                                            return ref;
                                        }});
    SequencerVoiceMap map;
    SfzLoader::PrepareSequencerVoices(map);
    ASSERT_EQ(map.tracks[0].count, 2);
    ASSERT_EQ(map.tracks[1].count, 2);
    const auto revision = map.tracks[1].revision;
    const auto resonance = map.tracks[1].zones[0].filter_resonance;
    InstrumentFilter filter;
    filter.resonance = 0.25f;
    ASSERT_TRUE(SfzLoader::SetInstrumentFilter(0, filter));
    filter.resonance = 0.5f;
    ASSERT_TRUE(SfzLoader::SetInstrumentFilter(1, filter));
    SfzLoader::PrepareSequencerVoices(map, 1);
    EXPECT_FLOAT_EQ(map.tracks[0].zones[0].filter_resonance, 0.25f);
    EXPECT_EQ(map.tracks[1].revision, revision);
    EXPECT_FLOAT_EQ(map.tracks[1].zones[0].filter_resonance, resonance);
    SfzLoader::PrepareSequencerVoices(map, 2);
    EXPECT_FLOAT_EQ(map.tracks[1].zones[0].filter_resonance, 0.5f);
}

TEST_F(SfzLoaderTest, PadSoundOverridesSurviveCopyAndReloadAndCanInheritAgain) {
    ASSERT_TRUE(Load(1));
    const auto a = SampleId("/kits/a.wav");
    Edit(INST_OP_NEW, 0, "Sounds");
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 0, a);
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 15, a);
    InstrumentEnv env;
    env.attack_s = .0234f;
    env.decay_s = .4567f;
    env.sustain = .61f;
    ASSERT_TRUE(SfzLoader::SetInstrumentEnv(0, env));
    InstPadSoundOpMessage request{900, 0, 15, PAD_SOUND_CUTOFF, a, 1234};
    ASSERT_TRUE(SfzLoader::OnPadSoundOp(request));
    EXPECT_FALSE(SfzLoader::OnPadSoundOp(request));  // replay never applies twice
    SfzLoader::PumpEditorReply();
    auto sound = WaveX::Comm::last_pad_sound;
    EXPECT_EQ(sound.own, 1);
    EXPECT_EQ(sound.cutoff_hz, 1234);
    EXPECT_EQ(sound.attack_ms, 23);
    EXPECT_EQ(sound.decay_ms, 457);
    EXPECT_EQ(sound.sustain, 610);
    request = {901, 0, 0, PAD_SOUND_GET, 0, 0};
    EXPECT_FALSE(SfzLoader::OnPadSoundOp(request));
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_pad_sound.own, 0);
    EXPECT_EQ(WaveX::Comm::last_pad_sound.cutoff_hz, 20000);
    EXPECT_EQ(WaveX::Comm::last_pad_sound.completed_request_id, 900u);
    ASSERT_EQ(Edit(INST_OP_SAVE, 0, "Sounds saved").error, INST_ERROR_NONE);
    ASSERT_TRUE(SfzLoader::Load(
        "0:/wavex/instruments/Sounds saved.wxi", 2, pool_, memory_, io_.data(), io_.size()));
    request = {902, 2, 15, PAD_SOUND_GET, 0, 0};
    SfzLoader::OnPadSoundOp(request);
    SfzLoader::PumpEditorReply();
    sound = WaveX::Comm::last_pad_sound;
    EXPECT_EQ(sound.own, 1);
    EXPECT_EQ(sound.cutoff_hz, 1234);
    EXPECT_EQ(sound.decay_ms, 457);
    request = {903, 2, 15, PAD_SOUND_INHERIT, sound.sample_id, 0};
    EXPECT_TRUE(SfzLoader::OnPadSoundOp(request));
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_pad_sound.own, 0);
    EXPECT_EQ(WaveX::Comm::last_pad_sound.cutoff_hz, 20000);
}
TEST_F(SfzLoaderTest, PadSoundRejectsEmptyStaleBusyAndMalformedEdits) {
    ASSERT_TRUE(Load(1));
    const auto a = SampleId("/kits/a.wav"), b = SampleId("/kits/b.wav");
    Edit(INST_OP_NEW, 0, "Sounds");
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 0, a);
    InstPadSoundOpMessage request{901, 0, 0, PAD_SOUND_ATTACK, b, 200};
    EXPECT_FALSE(SfzLoader::OnPadSoundOp(request));
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_pad_sound.error, INST_ERROR_BAD_FILE);
    request = {902, 0, 15, PAD_SOUND_ATTACK, a, 200};
    EXPECT_FALSE(SfzLoader::OnPadSoundOp(request));
    request = {903, 0, 0, PAD_SOUND_SUSTAIN, a, 1001};
    EXPECT_FALSE(SfzLoader::OnPadSoundOp(request));
    ASSERT_TRUE(SfzLoader::Begin({800, 0, INST_OP_SAVE, "Busy save"}));
    request = {904, 0, 0, PAD_SOUND_DECAY, a, 200};
    EXPECT_FALSE(SfzLoader::OnPadSoundOp(request));
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_pad_sound.error, INST_ERROR_BUSY);
    EXPECT_EQ(WaveX::Comm::last_pad_sound.own, 0);
    request = {905, 0, 0, PAD_SOUND_GET, 0, 0};
    SfzLoader::OnPadSoundOp(request);
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_pad_sound.completed_request_id, 904u);
    EXPECT_EQ(WaveX::Comm::last_pad_sound.error, INST_ERROR_BUSY);
}

TEST_F(SfzLoaderTest, PadSoundPreparationChangesOnlyFutureSnapshotOnSelectedTrack) {
    ASSERT_TRUE(Load(1));
    const auto a = SampleId("/kits/a.wav");
    Edit(INST_OP_NEW, 0, "Snapshot kit");
    Edit(INST_OP_SET_PAD_SAMPLE, 0, "", 15, a);
    SfzLoader::SetLoadedSampleResolver({nullptr, [](const void*, uint16_t) {
                                            static const int16_t pcm[64]{};
                                            SampleRef ref;
                                            ref.data = pcm;
                                            ref.frames = 64;
                                            ref.sample_rate_hz = 48000;
                                            return ref;
                                        }});
    SequencerVoiceMap map;
    SfzLoader::PrepareSequencerVoices(map);
    const auto other_revision = map.tracks[1].revision;
    VoiceTriggerParams old[4], updated[4];
    ASSERT_EQ(map.Resolve(0, 75, 100, old), 1);
    ASSERT_TRUE(SfzLoader::OnPadSoundOp({900, 0, 15, PAD_SOUND_CUTOFF, a, 777}));
    EXPECT_FLOAT_EQ(map.tracks[0].zones[0].filter_cutoff_hz, 20000);
    SfzLoader::PrepareSequencerVoices(map, 1);
    ASSERT_EQ(map.Resolve(0, 75, 100, updated), 1);
    EXPECT_FLOAT_EQ(old[0].filter_cutoff_hz, 20000);
    EXPECT_FALSE(old[0].own_filter_env);
    EXPECT_FLOAT_EQ(updated[0].filter_cutoff_hz, 777);
    EXPECT_TRUE(updated[0].own_filter_env);
    EXPECT_EQ(map.tracks[1].revision, other_revision);
}

namespace {
TEST_F(SfzLoaderTest, TrackReadbackReportsAuthoritativeRoutingAndCurrentBinding) {
    SfzLoader::OnTrackStateRequest({51, 15});
    SfzLoader::PumpEditorReply();
    auto s = WaveX::Comm::last_track_state;
    EXPECT_EQ(s.request_id, 51u);
    EXPECT_EQ(s.valid, 1);
    EXPECT_EQ(s.loaded, 0);
    EXPECT_EQ(s.midi_in, 16);
    ASSERT_TRUE(Load(15));
    ASSERT_TRUE(SfzLoader::SetTrackMidiIn(15, TRACK_MIDI_IN_OFF));
    ASSERT_TRUE(SfzLoader::SetTrackPriority(15, 99));
    SfzLoader::OnTrackStateRequest({52, 15});
    SfzLoader::PumpEditorReply();
    s = WaveX::Comm::last_track_state;
    EXPECT_EQ(s.request_id, 52u);
    EXPECT_EQ(s.loaded, 1);
    EXPECT_EQ(s.midi_in, TRACK_MIDI_IN_OFF);
    EXPECT_EQ(s.priority, 99);
    EXPECT_STREQ(s.name, "kit.sfz");
    EXPECT_EQ(SfzLoader::TrackMidiIn(0), 1);
    SfzLoader::OnTrackStateRequest({53, 16});
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_track_state.valid, 0);
    EXPECT_EQ(WaveX::Comm::last_track_state.request_id, 53u);
}
}  // namespace

namespace {
InstKeyMapSyncMessage KeyRead(uint8_t track, uint32_t id = 998, uint8_t oscillator = 0) {
    InstKeyMapOpMessage m;
    m.request_id = id;
    m.track = track;
    m.oscillator = oscillator;
    SfzLoader::OnKeyMapOp(m);
    SfzLoader::PumpEditorReply();
    return WaveX::Comm::last_key_map;
}

TEST_F(SfzLoaderTest, SecondMapEditsAreIndependentAndRetainCrossMapOwnership) {
    ASSERT_TRUE(Load(0));
    const auto first = KeyRead(0);
    const auto sample = first.zones[0].sample_id;
    InstKeyMapOpMessage m;
    m.request_id = 90001;
    m.revision = first.revision;
    m.oscillator = 1;
    m.zone = 31;
    m.op = KEY_MAP_ASSIGN;
    m.value.sample_id = sample;
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(m));
    EXPECT_EQ(SfzLoader::VoiceStopTrack(), 0);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    auto second = KeyRead(0, 90002, 1);
    EXPECT_EQ(second.zones[31].sample_id, sample);
    EXPECT_EQ(KeyRead(0).zones[31].sample_id, 0);
    EXPECT_EQ(KeyRead(0).zones[0].sample_id, sample);
    EXPECT_EQ(SfzLoader::ReadOscState(0, 1).type, static_cast<uint8_t>(OscType::Sample));
    m.request_id = 90003;
    m.revision = second.revision;
    m.op = KEY_MAP_SET_RANGE;
    m.expected_sample = sample;
    m.value = {sample, 40, 80, 64, 127, 60};
    ASSERT_TRUE(SfzLoader::OnKeyMapOp(m));
    second = KeyRead(0, 90004, 1);
    EXPECT_EQ(second.zones[31].vel_lo, 64);
    EXPECT_EQ(KeyRead(0).zones[0].vel_lo, first.zones[0].vel_lo);
    // Replacing one map invalidates an outstanding edit to the other.
    m.request_id = 90005;
    m.oscillator = 0;
    m.zone = 0;
    m.value = first.zones[0];
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(m));
    m.request_id = 90006;
    m.oscillator = 1;
    m.zone = 31;
    m.revision = second.revision;
    m.op = KEY_MAP_ASSIGN;
    m.value.sample_id = 0;
    SfzLoader::OnKeyMapOp(m);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    EXPECT_EQ(KeyRead(0, 90007, 1).zones[31].sample_id, 0);
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_TRUE(pool_.Find(sample)->used_by & 1);
    EXPECT_EQ(KeyRead(0).zones[0].sample_id, sample);
}

TEST_F(SfzLoaderTest, SavePreflightIncludesSparseSecondOscillatorWithoutStoppingVoices) {
    ASSERT_TRUE(Load(0));
    AddResidentWave("/kits/large.wav", WAVEX_INST_MAX_RAM_SAMPLE_BYTES + 4, 2);
    const auto sample = SampleId("/kits/large.wav");
    InstKeyMapOpMessage assign;
    assign.request_id = 91000;
    assign.revision = KeyRead(0).revision;
    assign.oscillator = 1;
    assign.zone = 31;
    assign.op = KEY_MAP_ASSIGN;
    assign.value.sample_id = sample;
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(assign));
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    ASSERT_EQ(KeyRead(0, 91001, 1).zones[31].sample_id, sample);

    ASSERT_TRUE(SfzLoader::Begin({91002, 0, INST_OP_SAVE, "Second source"}));
    unsigned passes = 0;
    while (SfzLoader::Busy() && passes < 100) {
        EXPECT_EQ(SfzLoader::VoiceStopTrack(), 0xFF);
        EXPECT_FALSE(SfzLoader::TrackLoading(0));
        SfzLoader::Pump(pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size()));
        ++passes;
    }
    EXPECT_FALSE(SfzLoader::Busy());
    // Snapshot plus one pass for each of the three populated dependencies.
    EXPECT_GE(passes, 4u);
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_pad_map.error, INST_ERROR_UNSUPPORTED_SAMPLE);
    EXPECT_EQ(WaveX::Comm::last_pad_map.completed_request_id, 91002u);
    EXPECT_EQ(MockFatFS::Instance().GetFile("0:/wavex/instruments/Second source.wxi"), nullptr);
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 1);
    EXPECT_EQ(pool_.Count(), 3u);
}

TEST_F(SfzLoaderTest, ModulatorEditsPreserveAllEnvelopesAndRoutesAcrossWxiRecall) {
    ASSERT_TRUE(Load(0));
    InstModOpMessage m;
    m.request_id = 99001;
    m.track = 0;
    m.revision = SfzLoader::ReadModState(0).revision;
    m.op = INST_MOD_SET_ENV;
    m.index = 2;
    m.envelope = {.023f, .45f, .67f, .89f};
    ASSERT_TRUE(SfzLoader::OnModOp(m));
    const auto applied = SfzLoader::ReadModState(0);
    EXPECT_FLOAT_EQ(applied.envelopes[2].attack_s, .023f);
    EXPECT_FALSE(SfzLoader::OnModOp(m));
    EXPECT_EQ(SfzLoader::ReadModState(0).revision, applied.revision);
    m.request_id++;
    m.index = 1;
    EXPECT_FALSE(SfzLoader::OnModOp(m));  // old revision
    EXPECT_EQ(SfzLoader::ReadModState(0).error, INST_ERROR_BAD_FILE);
    m.request_id++;
    m.revision = applied.revision;
    m.op = INST_MOD_SET_SLOT;
    m.index = 7;
    m.slot = {SRC_ENV_AUX, DEST_PITCH, -25000, CURVE_LINEAR, 0};
    ASSERT_TRUE(SfzLoader::OnModOp(m));
    EXPECT_EQ(SfzLoader::GetModSlots(0)[7].source, SRC_ENV_AUX);
    EXPECT_EQ(SfzLoader::VoiceStopTrack(), 0xFF);
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Three envelopes").error, INST_ERROR_NONE);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    ASSERT_TRUE(SfzLoader::Load("0:/wavex/instruments/Three envelopes.wxi",
                                0,
                                pool_,
                                memory_,
                                io_.data(),
                                static_cast<uint32_t>(io_.size())));
    const auto restored = SfzLoader::ReadModState(0);
    EXPECT_FLOAT_EQ(restored.envelopes[2].attack_s, .023f);
    EXPECT_FLOAT_EQ(restored.envelopes[2].release_s, .89f);
    EXPECT_EQ(restored.slots[7].source, SRC_ENV_AUX);
    EXPECT_EQ(restored.slots[7].depth, -25000);
    // A legacy amp edit must invalidate an outstanding touch snapshot.
    auto env = *SfzLoader::GetInstrumentEnv(0);
    env.attack_s = .1f;
    ASSERT_TRUE(SfzLoader::SetInstrumentEnv(0, env));
    m.request_id++;
    m.revision = restored.revision;
    EXPECT_FALSE(SfzLoader::OnModOp(m));
}
TEST_F(SfzLoaderTest, KeyboardRangesRejectStaleEditsAndKeepOtherZones) {
    ASSERT_TRUE(Load(0));
    auto state = KeyRead(0);
    ASSERT_EQ(state.loaded, 1);
    ASSERT_EQ(state.mode, 0);
    InstKeyMapOpMessage m;
    m.request_id = 1001;
    m.revision = state.revision;
    m.track = 0;
    m.zone = 0;
    m.op = KEY_MAP_SET_RANGE;
    m.expected_sample = state.zones[0].sample_id;
    m.value = {m.expected_sample, 30, 65, 1, 63, 48};
    ASSERT_TRUE(SfzLoader::OnKeyMapOp(m));
    state = KeyRead(0);
    EXPECT_EQ(state.zones[0].root_note, 48);
    EXPECT_EQ(state.zones[0].vel_hi, 63);
    EXPECT_EQ(state.zones[1].key_lo, 61);
    EXPECT_EQ(SfzLoader::VoiceStopTrack(), 255);
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(m));  // replay cannot increment revision
    EXPECT_EQ(KeyRead(0).revision, state.revision);
    m.request_id = 1002;  // stale generation, even though sample still matches
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(m));
    EXPECT_EQ(KeyRead(0).error, INST_ERROR_BAD_FILE);
    ASSERT_TRUE(Load(0));
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(m));
}
TEST_F(SfzLoaderTest, SparseKeyboardAssignmentStopsOnlyItsTrackAndRetainsSharedSamples) {
    ASSERT_TRUE(Load(1));
    const auto sample = SampleId("/kits/a.wav");
    ASSERT_EQ(Edit(INST_OP_NEW_KEYBOARD, 0, "Keys").error, INST_ERROR_NONE);
    auto state = KeyRead(0);
    ASSERT_EQ(state.mode, 0);
    ASSERT_EQ(state.loaded, 1);
    InstKeyMapOpMessage m;
    m.request_id = 2001;
    m.revision = state.revision;
    m.track = 0;
    m.zone = 31;
    m.op = KEY_MAP_ASSIGN;
    m.value.sample_id = sample;
    SfzLoader::OnKeyMapOp(m);
    EXPECT_EQ(SfzLoader::VoiceStopTrack(), 0);
    EXPECT_EQ(KeyRead(0).zones[31].sample_id, 0);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    state = KeyRead(0);
    EXPECT_EQ(state.zones[31].sample_id, sample);
    EXPECT_EQ(pool_.Find(sample)->used_by, 3);
    m.request_id = 2002;
    m.revision = state.revision;
    m.expected_sample = sample;
    m.value.sample_id = 0;
    SfzLoader::OnKeyMapOp(m);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    EXPECT_EQ(KeyRead(0).zones[31].sample_id, 0);
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 2);
}
TEST_F(SfzLoaderTest, KeyAssignmentRechecksRevisionAfterVoiceBarrier) {
    ASSERT_TRUE(Load(0));
    auto state = KeyRead(0);
    InstKeyMapOpMessage m;
    m.request_id = 3001;
    m.revision = state.revision;
    m.op = KEY_MAP_ASSIGN;
    m.expected_sample = state.zones[0].sample_id;
    m.value.sample_id = state.zones[1].sample_id;
    SfzLoader::OnKeyMapOp(m);
    SfzLoader::ForgetLoadedSample(m.expected_sample);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    EXPECT_EQ(KeyRead(0).error, INST_ERROR_BAD_FILE);
    EXPECT_EQ(KeyRead(0).zones[0].sample_id, 0);
}
}  // namespace

namespace {
TEST_F(SfzLoaderTest, SparseKeyboardRangesSaveReloadAndPublishOnlyFutureResolution) {
    ASSERT_TRUE(Load(1));
    const auto sample = SampleId("/kits/a.wav");
    ASSERT_EQ(Edit(INST_OP_NEW_KEYBOARD, 0, "Split").error, INST_ERROR_NONE);
    auto state = KeyRead(0);
    InstKeyMapOpMessage m;
    m.request_id = 4001;
    m.revision = state.revision;
    m.zone = 31;
    m.op = KEY_MAP_ASSIGN;
    m.value.sample_id = sample;
    SfzLoader::OnKeyMapOp(m);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    state = KeyRead(0);
    SfzLoader::SetLoadedSampleResolver({nullptr, [](const void*, uint16_t) {
                                            static const int16_t pcm[4]{};
                                            SampleRef ref;
                                            ref.data = pcm;
                                            ref.frames = 4;
                                            ref.sample_rate_hz = 48000;
                                            return ref;
                                        }});
    SequencerVoiceMap previous, updated;
    SfzLoader::PrepareSequencerVoices(previous);
    m.request_id = 4002;
    m.revision = state.revision;
    m.op = KEY_MAP_SET_RANGE;
    m.expected_sample = sample;
    m.value = {sample, 24, 72, 64, 127, 48};
    ASSERT_TRUE(SfzLoader::OnKeyMapOp(m));
    SfzLoader::PrepareSequencerVoices(updated);
    VoiceTriggerParams triggers[4];
    EXPECT_EQ(previous.Resolve(0, 60, 63, triggers), 1);
    EXPECT_EQ(updated.Resolve(0, 60, 63, triggers), 0);
    EXPECT_EQ(updated.Resolve(0, 24, 64, triggers), 1);
    EXPECT_EQ(updated.Resolve(0, 72, 127, triggers), 1);
    EXPECT_EQ(updated.Resolve(0, 73, 100, triggers), 0);
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Key split saved").error, INST_ERROR_NONE);
    SfzLoader::BindSample(pool_, memory_, 0, 0);
    SfzLoader::BindSample(pool_, memory_, 1, 0);
    ASSERT_EQ(pool_.Count(), 0u);
    ASSERT_TRUE(SfzLoader::Load(
        "0:/wavex/instruments/Key split saved.wxi", 2, pool_, memory_, io_.data(), io_.size()));
    state = KeyRead(2);
    EXPECT_EQ(state.zones[0].sample_id, 0);
    EXPECT_NE(state.zones[31].sample_id, 0);
    EXPECT_EQ(state.zones[31].key_lo, 24);
    EXPECT_EQ(state.zones[31].key_hi, 72);
    EXPECT_EQ(state.zones[31].vel_lo, 64);
    EXPECT_EQ(state.zones[31].vel_hi, 127);
    EXPECT_EQ(state.zones[31].root_note, 48);
}
}  // namespace

namespace {
TEST_F(SfzLoaderTest, NamedAndSplitKeyboardMapsAreInstrumentBindings) {
    ASSERT_TRUE(Load(1));
    const auto sample = SampleId("/kits/a.wav");
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, sample));
    EXPECT_EQ(SfzLoader::BoundSample(0), sample);
    auto state = KeyRead(0);
    InstKeyMapOpMessage m;
    m.request_id = 5101;
    m.revision = state.revision;
    m.op = KEY_MAP_SET_RANGE;
    m.expected_sample = sample;
    m.value = {sample, 24, 72, 1, 127, 60};
    ASSERT_TRUE(SfzLoader::OnKeyMapOp(m));
    EXPECT_EQ(SfzLoader::BoundSample(0), 0);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, sample));
    EXPECT_EQ(Edit(INST_OP_SET_NAME, 0, "Named keys").error, INST_ERROR_NONE);
    EXPECT_EQ(SfzLoader::BoundSample(0), 0);
}
}  // namespace

TEST_F(SfzLoaderTest, ClearingOscillatorOneRetainsSamplesStillOwnedByOscillatorTwo) {
    WaveX::Wxi::InstrumentFile doc;
    std::strcpy(doc.name, "Dual map");
    doc.osc[0].type = WaveX::Wxi::OscType::Sample;
    doc.osc[1].type = WaveX::Wxi::OscType::Sample;
    doc.osc[0].zone_count = 1;
    doc.osc[1].zone_count = 2;
    std::strcpy(doc.osc[0].zones[0].path, "/kits/a.wav");
    std::strcpy(doc.osc[1].zones[0].path, "/kits/a.wav");
    doc.osc[1].zones[1].index = 31;
    std::strcpy(doc.osc[1].zones[1].path, "/kits/b.wav");
    std::vector<uint8_t> bytes;
    WaveX::Wxcf::IoContext io{&bytes,
                              nullptr,
                              [](void* context, const void* source, size_t count) {
                                  auto& destination = *static_cast<std::vector<uint8_t>*>(context);
                                  const auto* first = static_cast<const uint8_t*>(source);
                                  destination.insert(destination.end(), first, first + count);
                                  return true;
                              },
                              nullptr};
    ASSERT_EQ(WaveX::Wxi::Write(io, doc), WaveX::Wxi::Result::Ok);
    MockFatFS::Instance().AddFile("/kits/dual.wxi", bytes);
    ASSERT_TRUE(SfzLoader::Load(
        "/kits/dual.wxi", 0, pool_, memory_, io_.data(), static_cast<uint32_t>(io_.size())));
    ASSERT_EQ(pool_.Count(), 2u);
    const auto sample = SampleId("/kits/a.wav");
    auto state = KeyRead(0);
    InstKeyMapOpMessage clear;
    clear.request_id = 3001;
    clear.revision = state.revision;
    clear.track = 0;
    clear.zone = 0;
    clear.op = KEY_MAP_ASSIGN;
    clear.expected_sample = sample;
    clear.value.sample_id = 0;
    EXPECT_FALSE(SfzLoader::OnKeyMapOp(clear));  // queued; not yet applied
    ASSERT_EQ(SfzLoader::VoiceStopTrack(), 0);
    SfzLoader::ConfirmVoicesStopped(pool_, memory_);
    EXPECT_EQ(KeyRead(0).zones[0].sample_id, 0);
    ASSERT_NE(pool_.Find(sample), nullptr);
    EXPECT_EQ(pool_.Find(sample)->used_by, 1);
    EXPECT_EQ(SfzLoader::BoundSample(0), 0);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    EXPECT_EQ(pool_.Count(), 0u);
}

TEST_F(SfzLoaderTest, OscillatorCopyAndSettingsPreserveOwnershipAndRejectStaleEdits) {
    ASSERT_TRUE(Load(0));
    InstOscOpMessage request;
    request.request_id = 4000;
    request.oscillator = 1;
    EXPECT_FALSE(SfzLoader::OnOscOp(request));
    SfzLoader::PumpEditorReply();
    auto reply = WaveX::Comm::last_osc;
    ASSERT_EQ(reply.valid, 1);
    ASSERT_EQ(reply.zones, 0);
    request.request_id++;
    request.revision = reply.revision;
    request.op = INST_OSC_COPY_EMPTY;
    ASSERT_TRUE(SfzLoader::OnOscOp(request));
    SfzLoader::PumpEditorReply();
    reply = WaveX::Comm::last_osc;
    EXPECT_EQ(reply.zones, 2);
    EXPECT_EQ(reply.type, 1);
    EXPECT_EQ(pool_.Count(), 2u);
    EXPECT_EQ(SfzLoader::VoiceStopTrack(), 0xFF);  // no old source was invalidated
    ASSERT_NE(reply.revision, request.revision);
    EXPECT_FALSE(SfzLoader::OnOscOp(request));  // duplicate delivery is idempotent
    request.request_id++;
    request.op = INST_OSC_SET;
    request.value = {0.75f, 0.5f, 12, -25, 0, 0};
    EXPECT_FALSE(SfzLoader::OnOscOp(request));  // stale revision
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_osc.error, INST_ERROR_BAD_FILE);
    request.request_id++;
    request.revision = reply.revision;
    ASSERT_TRUE(SfzLoader::OnOscOp(request));
    SfzLoader::PumpEditorReply();
    reply = WaveX::Comm::last_osc;
    EXPECT_FLOAT_EQ(reply.value.mix, 0.5f);
    EXPECT_FLOAT_EQ(reply.value.level, 0.75f);
    EXPECT_EQ(reply.value.coarse, 12);
    EXPECT_EQ(reply.value.keytrack, 0);
    request.request_id++;
    request.revision = reply.revision;
    request.op = INST_OSC_COPY_EMPTY;
    EXPECT_FALSE(SfzLoader::OnOscOp(request));  // populated destination is preserved
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_osc.error, INST_ERROR_EXISTS);
    ASSERT_NE(pool_.Find(SampleId("/kits/a.wav")), nullptr);
    EXPECT_EQ(pool_.Find(SampleId("/kits/a.wav"))->used_by, 1);
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Dual settings").error, INST_ERROR_NONE);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    EXPECT_EQ(pool_.Count(), 0u);
    ASSERT_TRUE(SfzLoader::Load("0:/wavex/instruments/Dual settings.wxi",
                                0,
                                pool_,
                                memory_,
                                io_.data(),
                                static_cast<uint32_t>(io_.size())));
    request.request_id++;
    request.op = INST_OSC_GET;
    SfzLoader::OnOscOp(request);
    SfzLoader::PumpEditorReply();
    EXPECT_EQ(WaveX::Comm::last_osc.zones, 2);
    EXPECT_FLOAT_EQ(WaveX::Comm::last_osc.value.mix, 0.5f);
    EXPECT_EQ(WaveX::Comm::last_osc.value.fine, -25);
}

TEST_F(SfzLoaderTest, TwoLfoSettingsUseTrackRevisionsAndSurviveWxiRecall) {
    ASSERT_TRUE(Load(0));
    InstLfoOpMessage m;
    m.request_id = 99501;
    m.track = 0;
    m.index = 1;
    m.op = INST_LFO_SET;
    m.revision = SfzLoader::ReadLfoState(0).revision;
    m.value = {4, 0, 0, 1, 5.25f, .125f, .75f};
    ASSERT_TRUE(SfzLoader::OnLfoOp(m));
    auto state = SfzLoader::ReadLfoState(0);
    EXPECT_FLOAT_EQ(state.values[1].rate_hz, 5.25f);
    EXPECT_FALSE(SfzLoader::OnLfoOp(m));
    EXPECT_EQ(SfzLoader::ReadLfoState(0).revision, state.revision);
    ++m.request_id;
    EXPECT_FALSE(SfzLoader::OnLfoOp(m));
    EXPECT_EQ(SfzLoader::ReadLfoState(0).error, INST_ERROR_BAD_FILE);
    ++m.request_id;
    m.revision = state.revision;
    m.index = 0;
    m.value = {1, 7, 1, 0, 3, .025f, .25f};
    ASSERT_TRUE(SfzLoader::OnLfoOp(m));
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Two LFOs").error, INST_ERROR_NONE);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, 0));
    ASSERT_TRUE(SfzLoader::Load("0:/wavex/instruments/Two LFOs.wxi",
                                0,
                                pool_,
                                memory_,
                                io_.data(),
                                static_cast<uint32_t>(io_.size())));
    state = SfzLoader::ReadLfoState(0);
    EXPECT_EQ(state.values[0].sync_div, 7);
    EXPECT_EQ(state.values[1].pitch_follow, 1);
    EXPECT_EQ(state.values[1].retrigger, 0);
    EXPECT_FLOAT_EQ(state.values[1].rate_hz, 5.25f);
    EXPECT_FLOAT_EQ(state.values[1].fade_s, .75f);
}

TEST_F(SfzLoaderTest, SoundEditsAuditionAndRevertAcrossControlsWithoutChangingZones) {
    ASSERT_TRUE(Load(0));
    const auto baseline = SfzLoader::ReadOscState(0, 0);
    const auto env_before = SfzLoader::ReadModState(0);
    InstOscOpMessage osc;
    osc.request_id = 920001;
    osc.revision = baseline.revision;
    osc.op = INST_OSC_SET;
    osc.value = baseline.value;
    osc.value.level = .25f;
    ASSERT_TRUE(SfzLoader::OnOscOp(osc));
    EXPECT_FLOAT_EQ(SfzLoader::ReadOscState(0, 0).value.level, .25f);
    EXPECT_TRUE(SfzLoader::ReadEditState(0).dirty);
    InstModOpMessage env;
    env.request_id = 920002;
    env.revision = SfzLoader::ReadEditState(0).revision;
    env.op = INST_MOD_SET_ENV;
    env.index = 2;
    env.envelope.attack_s = 2.5f;
    ASSERT_TRUE(SfzLoader::OnModOp(env));
    InstEditOpMessage revert;
    revert.request_id = 920003;
    revert.revision = SfzLoader::ReadEditState(0).revision;
    revert.op = INST_EDIT_REVERT;
    ASSERT_TRUE(SfzLoader::OnEditOp(revert));
    EXPECT_FALSE(SfzLoader::ReadEditState(0).dirty);
    EXPECT_FLOAT_EQ(SfzLoader::ReadOscState(0, 0).value.level, baseline.value.level);
    EXPECT_FLOAT_EQ(SfzLoader::ReadModState(0).envelopes[2].attack_s,
                    env_before.envelopes[2].attack_s);
    EXPECT_EQ(SfzLoader::ReadOscState(0, 0).zones, baseline.zones);
    const auto revision = SfzLoader::ReadEditState(0).revision;
    EXPECT_FALSE(SfzLoader::OnEditOp(revert));
    EXPECT_EQ(SfzLoader::ReadEditState(0).revision, revision);
}
TEST_F(SfzLoaderTest, ApplyBecomesRevertPointAndStaleActionsCannotConsumeNewEdits) {
    ASSERT_TRUE(Load(0));
    InstEditOpMessage op;
    op.request_id = 921001;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_AMP;
    op.sound.gain = .5f;
    op.sound.pan = .25f;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    op.request_id++;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_APPLY;
    EXPECT_FALSE(SfzLoader::OnEditOp(op));
    EXPECT_FALSE(SfzLoader::ReadEditState(0).dirty);
    const auto applied = op;
    op.request_id++;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_AMP;
    op.sound.gain = 0;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    EXPECT_TRUE(SfzLoader::ReadEditState(0).dirty);
    EXPECT_FALSE(SfzLoader::OnEditOp(applied));
    EXPECT_TRUE(SfzLoader::ReadEditState(0).dirty);
    op.request_id++;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_REVERT;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    EXPECT_FLOAT_EQ(SfzLoader::ReadEditState(0).sound.gain, .5f);
    EXPECT_FLOAT_EQ(SfzLoader::ReadEditState(0).sound.pan, .25f);
}
TEST_F(SfzLoaderTest, FilterSettingsEditCarriesTheTopologyAndLegacyEditsKeepIt) {
    ASSERT_TRUE(Load(0));
    EXPECT_EQ(SfzLoader::ReadEditState(0).filter_topology, INST_FILTER_TOPOLOGY_SVF);
    InstEditOpMessage op;
    op.request_id = 923001;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_FILTER_SETTINGS;
    op.filter_type = INST_FILTER_BP;
    op.filter_topology = INST_FILTER_TOPOLOGY_LADDER;
    op.filter_slope = INST_FILTER_SLOPE_24;
    op.filter_drive = .5f;
    op.sound = SfzLoader::ReadEditState(0).sound;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    auto state = SfzLoader::ReadEditState(0);
    EXPECT_EQ(state.filter_type, INST_FILTER_BP);
    EXPECT_EQ(state.filter_topology, INST_FILTER_TOPOLOGY_LADDER);
    EXPECT_EQ(state.filter_slope, INST_FILTER_SLOPE_24);
    EXPECT_FLOAT_EQ(state.filter_drive, .5f);
    EXPECT_TRUE(state.dirty);
    EXPECT_EQ(SfzLoader::GetInstrumentFilter(0)->topology, INST_FILTER_TOPOLOGY_LADDER);
    EXPECT_FLOAT_EQ(SfzLoader::GetInstrumentFilter(0)->drive, .5f);
    // A legacy cutoff edit carries zero bytes and must not put the SVF back.
    op.request_id++;
    op.revision = state.revision;
    op.op = INST_EDIT_FILTER;
    op.filter_type = 0;
    op.filter_topology = 0;
    op.filter_slope = 0;
    op.filter_drive = 0;
    op.sound.cutoff_hz = 1234;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    state = SfzLoader::ReadEditState(0);
    EXPECT_EQ(state.filter_topology, INST_FILTER_TOPOLOGY_LADDER);
    EXPECT_EQ(state.filter_type, INST_FILTER_BP);
    EXPECT_EQ(state.filter_slope, INST_FILTER_SLOPE_24);
    EXPECT_FLOAT_EQ(state.filter_drive, .5f);
    EXPECT_FLOAT_EQ(state.sound.cutoff_hz, 1234);
    // Re-sending the same settings is not an edit.
    op.request_id++;
    op.revision = state.revision;
    op.op = INST_EDIT_FILTER_SETTINGS;
    op.filter_type = INST_FILTER_BP;
    op.filter_topology = INST_FILTER_TOPOLOGY_LADDER;
    op.filter_slope = INST_FILTER_SLOPE_24;
    op.filter_drive = .5f;
    EXPECT_FALSE(SfzLoader::OnEditOp(op));
    op.request_id++;
    op.revision = SfzLoader::ReadEditState(0).revision;  // a no-op still advances it
    op.op = INST_EDIT_REVERT;
    op.filter_type = 0;  // only op 5 may carry these
    op.filter_topology = 0;
    op.filter_slope = 0;
    op.filter_drive = 0;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    state = SfzLoader::ReadEditState(0);
    EXPECT_EQ(state.filter_topology, INST_FILTER_TOPOLOGY_SVF);
    EXPECT_EQ(state.filter_type, INST_FILTER_LP);
    EXPECT_EQ(state.filter_slope, INST_FILTER_SLOPE_12);
    EXPECT_FLOAT_EQ(state.filter_drive, 0);
    EXPECT_FALSE(state.dirty);
}
TEST_F(SfzLoaderTest, SuccessfulSaveAppliesAudibleSettingsButFailedSaveKeepsUndo) {
    ASSERT_TRUE(Load(0));
    InstEditOpMessage op;
    op.request_id = 922001;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_FILTER;
    op.sound.cutoff_hz = 1234;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    ASSERT_EQ(Edit(INST_OP_SAVE, 0, "Audible").error, INST_ERROR_NONE);
    EXPECT_FALSE(SfzLoader::ReadEditState(0).dirty);
    op.request_id++;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.sound.cutoff_hz = 4321;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    EXPECT_EQ(Edit(INST_OP_SAVE, 0, "Audible").error, INST_ERROR_EXISTS);
    EXPECT_TRUE(SfzLoader::ReadEditState(0).dirty);
    op.request_id++;
    op.revision = SfzLoader::ReadEditState(0).revision;
    op.op = INST_EDIT_REVERT;
    ASSERT_TRUE(SfzLoader::OnEditOp(op));
    EXPECT_FLOAT_EQ(SfzLoader::ReadEditState(0).sound.cutoff_hz, 1234);
    ASSERT_TRUE(SfzLoader::BindSample(pool_, memory_, 0, SampleId("/kits/a.wav")));
    EXPECT_FALSE(SfzLoader::ReadEditState(0).dirty);
}

namespace {
TEST_F(SfzLoaderTest, ProjectStagingFailurePreservesLiveTracksUndoAndPcm) {
    ASSERT_TRUE(Load(0));
    // Preserve an actual live edit and its Revert point.
    InstEditOpMessage edit{};
    edit.request_id = ++next_request_;
    edit.track = 0;
    edit.op = INST_EDIT_FILTER;
    edit.revision = SfzLoader::ReadEditState(0).revision;
    edit.sound.cutoff_hz = 1234;
    ASSERT_TRUE(SfzLoader::OnEditOp(edit));
    ModSlot modulation;
    modulation.source = SRC_LFO1;
    modulation.dest = DEST_CUTOFF;
    modulation.depth = 1234;
    ASSERT_TRUE(SfzLoader::SetModSlot(0, 0, modulation));
    ASSERT_EQ(SfzLoader::GetModSlots(0)[0].depth, 1234);
    const auto before = SfzLoader::ReadEditState(0);
    const uint16_t old_a = SampleId("/kits/a.wav");
    const auto old_handle = pool_.Find(old_a)->payload.handle;
    wxsamp_stats_t initial{};
    memory_.stats(&initial);
    std::vector<SamplePool::Record> candidate_records(WAVEX_SAMPLE_POOL_CAPACITY);
    SamplePool candidate(candidate_records.data());
    SamplePoolStage pool_stage(pool_, candidate, memory_);
    Tracks tracks;
    ASSERT_TRUE(pool_stage.Begin());
    ASSERT_TRUE(SfzLoader::BeginProjectLoad(tracks));
    ASSERT_TRUE(SfzLoader::Busy());
    ASSERT_FALSE(SfzLoader::Begin(InstOpMessage(200, 0, INST_OP_SFZ_LOAD, "/kits/kit.sfz")));
    const char fresh[] = "<region> sample=new.wav key=60\n";
    MockFatFS::Instance().AddFile("/kits/new.sfz", {fresh, fresh + std::strlen(fresh)});
    MockFatFS::Instance().AddFile("/kits/new.wav", PcmWave());
    ASSERT_TRUE(SfzLoader::BeginProjectTrack(0, "/kits/new.sfz"));
    for (int i = 0; i < 1000 && SfzLoader::ProjectTrackBusy(); ++i)
        SfzLoader::PumpProjectLoad(candidate, memory_, io_.data(), io_.size());
    ASSERT_FALSE(SfzLoader::ProjectTrackBusy());
    ASSERT_EQ(SfzLoader::ProjectTrackError(), INST_ERROR_NONE);
    ASSERT_NE(candidate.FindByPath("/kits/new.wav"), nullptr);
    EXPECT_EQ(pool_.FindByPath("/kits/new.wav"), nullptr);
    EXPECT_STREQ(SfzLoader::TrackName(0), "kit.sfz");
    EXPECT_STREQ(tracks.At(0).instrument.name, "new.sfz");
    EXPECT_EQ(SfzLoader::GetModSlots(0)[0].depth, 1234);
    EXPECT_EQ(SfzLoader::ReadEditState(0).revision, before.revision);
    EXPECT_EQ(SfzLoader::ReadEditState(0).dirty, before.dirty);
    // Reusing a staged Track would let the ordinary loader free borrowed PCM.
    EXPECT_FALSE(SfzLoader::BeginProjectTrack(0, "/kits/kit.sfz"));
    ASSERT_TRUE(SfzLoader::BeginProjectTrack(1, "/kits/missing.sfz"));
    for (int i = 0; i < 1000 && SfzLoader::ProjectTrackBusy(); ++i)
        SfzLoader::PumpProjectLoad(candidate, memory_, io_.data(), io_.size());
    ASSERT_NE(SfzLoader::ProjectTrackError(), INST_ERROR_NONE);
    EXPECT_FALSE(SfzLoader::FinishProjectLoad(true));
    ASSERT_TRUE(SfzLoader::FinishProjectLoad(false));
    pool_stage.Rollback();
    EXPECT_EQ(pool_.Find(old_a)->used_by, 1);
    void* pcm = nullptr;
    EXPECT_TRUE(memory_.ptr(old_handle, &pcm));
    EXPECT_STREQ(SfzLoader::TrackName(0), "kit.sfz");
    EXPECT_EQ(SfzLoader::ReadEditState(0).revision, before.revision);
    EXPECT_EQ(SfzLoader::ReadEditState(0).dirty, before.dirty);
    wxsamp_stats_t after{};
    memory_.stats(&after);
    EXPECT_EQ(after.large_free_bytes, initial.large_free_bytes);
    EXPECT_EQ(after.small_free_bytes, initial.small_free_bytes);
}
TEST_F(SfzLoaderTest, ProjectCommitRetainsSharedAndPinnedPcmAndPublishesOnlyAtFinish) {
    ASSERT_TRUE(Load(0));
    const uint16_t a = SampleId("/kits/a.wav");
    const uint16_t b = SampleId("/kits/b.wav");
    AddResidentWave("/kits/pinned.wav", 8);
    const uint16_t pinned = SampleId("/kits/pinned.wav");
    pool_.SetPinned(pinned, true);
    std::vector<SamplePool::Record> candidate_records(WAVEX_SAMPLE_POOL_CAPACITY);
    SamplePool candidate(candidate_records.data());
    SamplePoolStage pool_stage(pool_, candidate, memory_);
    Tracks tracks;
    ASSERT_TRUE(pool_stage.Begin());
    ASSERT_TRUE(SfzLoader::BeginProjectLoad(tracks));
    const char shared[] = "<region> sample=a.wav key=60\n";
    MockFatFS::Instance().AddFile("/kits/shared.sfz", {shared, shared + std::strlen(shared)});
    ASSERT_TRUE(SfzLoader::BeginProjectTrack(15, "/kits/shared.sfz"));
    for (int i = 0; i < 1000 && SfzLoader::ProjectTrackBusy(); ++i)
        SfzLoader::PumpProjectLoad(candidate, memory_, io_.data(), io_.size());
    ASSERT_EQ(SfzLoader::ProjectTrackError(), INST_ERROR_NONE);
    EXPECT_FALSE(SfzLoader::TrackLoaded(15));
    EXPECT_EQ(pool_.Find(a)->used_by, 1);
    EXPECT_EQ(candidate.Find(a)->used_by, 0x8000);
    // Host model's stop fence: no callback runs in this fixture.
    ASSERT_TRUE(pool_stage.Commit());
    ASSERT_TRUE(SfzLoader::FinishProjectLoad(true));
    EXPECT_FALSE(SfzLoader::TrackLoaded(0));
    EXPECT_TRUE(SfzLoader::TrackLoaded(15));
    EXPECT_STREQ(SfzLoader::TrackName(15), "shared.sfz");
    EXPECT_EQ(pool_.Find(a)->used_by, 0x8000);
    EXPECT_EQ(pool_.Find(b), nullptr);  // unpinned old dependency retired
    ASSERT_NE(pool_.Find(pinned), nullptr);
    EXPECT_EQ(pool_.Find(pinned)->used_by, 0);
    EXPECT_TRUE(pool_.Find(pinned)->pinned);
    EXPECT_FALSE(SfzLoader::Busy());
}
}  // namespace

namespace {
TEST_F(SfzLoaderTest, ProjectCloseFailureAndCancellationDoNotReleaseBorrowedSamples) {
    ASSERT_TRUE(Load(0));
    const uint16_t old = SampleId("/kits/a.wav");
    std::vector<SamplePool::Record> candidate_records(WAVEX_SAMPLE_POOL_CAPACITY);
    SamplePool candidate(candidate_records.data());
    SamplePoolStage pool_stage(pool_, candidate, memory_);
    Tracks tracks;
    ASSERT_TRUE(pool_stage.Begin());
    ASSERT_TRUE(SfzLoader::BeginProjectLoad(tracks));
    ASSERT_TRUE(SfzLoader::BeginProjectTrack(1, "/kits/kit.sfz"));
    MockFatFS::Instance().read_close_result = FR_DISK_ERR;
    for (int i = 0; i < 1000 && SfzLoader::ProjectTrackBusy(); ++i)
        SfzLoader::PumpProjectLoad(candidate, memory_, io_.data(), io_.size());
    EXPECT_EQ(SfzLoader::ProjectTrackError(), INST_ERROR_IO);
    EXPECT_FALSE(SfzLoader::FinishProjectLoad(true));
    ASSERT_TRUE(SfzLoader::FinishProjectLoad(false));
    pool_stage.Rollback();
    MockFatFS::Instance().read_close_result = FR_OK;
    ASSERT_TRUE(pool_stage.Begin());
    ASSERT_TRUE(SfzLoader::BeginProjectLoad(tracks));
    const char sfz[] = "<region> sample=cancel.wav key=60\n";
    MockFatFS::Instance().AddFile("/kits/cancel.sfz", {sfz, sfz + std::strlen(sfz)});
    MockFatFS::Instance().AddFile("/kits/cancel.wav", PcmWave(65536));
    wxsamp_stats_t before{};
    memory_.stats(&before);
    ASSERT_TRUE(SfzLoader::BeginProjectTrack(2, "/kits/cancel.sfz"));
    for (int i = 0; i < 1000 && !candidate.FindByPath("/kits/cancel.wav"); ++i)
        SfzLoader::PumpProjectLoad(candidate, memory_, io_.data(), io_.size());
    ASSERT_NE(candidate.FindByPath("/kits/cancel.wav"), nullptr);
    ASSERT_TRUE(SfzLoader::ProjectTrackBusy());
    SfzLoader::CancelProjectTrack(candidate, memory_);
    wxsamp_stats_t after{};
    memory_.stats(&after);
    EXPECT_EQ(after.large_free_bytes, before.large_free_bytes);
    EXPECT_EQ(after.small_free_bytes, before.small_free_bytes);
    EXPECT_FALSE(SfzLoader::ProjectTrackBusy());
    ASSERT_TRUE(SfzLoader::FinishProjectLoad(false));
    pool_stage.Rollback();
    ASSERT_NE(pool_.Find(old), nullptr);
    EXPECT_EQ(pool_.Find(old)->used_by, 1);
    EXPECT_TRUE(SfzLoader::TrackLoaded(0));
    EXPECT_FALSE(SfzLoader::TrackLoaded(1));
    EXPECT_FALSE(SfzLoader::TrackLoaded(2));
}
}  // namespace
