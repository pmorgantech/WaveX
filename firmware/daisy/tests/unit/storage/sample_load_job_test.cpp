#include "storage/sample_load_job.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include "storage/sample_file_job.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace WaveX;
using namespace WaveX::AudioEngine;
using namespace WaveX::Protocol;
alignas(32) std::array<uint8_t, 512 * 1024> arena;
std::array<SamplePool::Record, WAVEX_SAMPLE_POOL_CAPACITY> records;

class SampleLoadJobTest : public testing::Test {
   protected:
    SamplePool pool{records.data()};
    SampleMemMgr memory;
    Storage::SampleLoadJob job;
    alignas(32) std::array<uint8_t, 8192> io{};
    SampleLoadMessage request;
    std::vector<uint8_t> wave;
    void SetUp() override {
        MockFatFS::Instance().Reset();
        ASSERT_TRUE(memory.init(arena.data(), arena.size(), 64 * 1024));
        std::strcpy(request.path, "/source.wav");
        request.sample_id = 123;
        wave.resize(44 + 20000);
        std::memcpy(wave.data(), "RIFF", 4);
        std::memcpy(wave.data() + 8, "WAVEfmt ", 8);
        using namespace Wxcf::detail;
        WriteU32LE(wave.data() + 4, static_cast<uint32_t>(wave.size() - 8));
        WriteU32LE(wave.data() + 16, 16);
        WriteU16LE(wave.data() + 20, 1);
        WriteU16LE(wave.data() + 22, 2);
        WriteU32LE(wave.data() + 24, 48000);
        WriteU32LE(wave.data() + 28, 192000);
        WriteU16LE(wave.data() + 32, 4);
        WriteU16LE(wave.data() + 34, 16);
        std::memcpy(wave.data() + 36, "data", 4);
        WriteU32LE(wave.data() + 40, 20000);
        for (size_t i = 44; i < wave.size(); ++i)
            wave[i] = static_cast<uint8_t>(i);
        MockFatFS::Instance().AddFile(request.path, wave);
    }
    void TearDown() override { job.Cancel(memory); }
    void Pump() { job.Pump(pool, memory, io.data(), static_cast<uint32_t>(io.size())); }
    void Finish() {
        for (int i = 0; i < 100 && job.Busy(); ++i)
            Pump();
        ASSERT_FALSE(job.Busy());
        EXPECT_TRUE(job.ReplyPending());
    }
    void ReachPayload() {
        ASSERT_TRUE(job.Begin(request));
        for (int i = 0; i < 4; ++i)
            Pump();
        ASSERT_TRUE(job.Busy());
        ASSERT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_PROGRESS);
    }
    void ExpectNoAllocation() {
        wxsamp_stats_t stats{};
        memory.stats(&stats);
        EXPECT_EQ(stats.objects_alive, 0u);
        EXPECT_EQ(pool.Count(), 0u);
    }
};

TEST_F(SampleLoadJobTest, YieldsBetweenReadsAndPublishesOnlyCompletePcm) {
    ASSERT_TRUE(job.Begin(request));
    EXPECT_EQ(pool.Count(), 0u);
    unsigned payload_passes = 0;
    for (int i = 0; i < 100 && job.Busy(); ++i) {
        const auto before = job.BytesRead();
        Pump();
        EXPECT_LE(job.BytesRead() - before, Storage::SampleLoadJob::kReadBytes);
        payload_passes += job.BytesRead() != before;
        if (job.Busy()) {
            EXPECT_EQ(pool.Count(), 0u);
            EXPECT_EQ(pool.FindByPath(request.path), nullptr);
        }
    }
    ASSERT_FALSE(job.Busy());
    EXPECT_EQ(payload_passes, 5u);
    ASSERT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_COMPLETE);
    ASSERT_EQ(pool.Count(), 1u);
    auto* record = pool.Find(job.Status().sample_id);
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->pinned);
    EXPECT_EQ(record->payload.meta.total_frames, 5000u);
    void* pcm = nullptr;
    ASSERT_TRUE(memory.ptr(record->payload.handle, &pcm));
    EXPECT_EQ(std::memcmp(pcm, wave.data() + 44, 20000), 0);
    EXPECT_FALSE(job.Begin(request));  // retain terminal reply under TX backpressure
    job.ReplySent();
    ASSERT_TRUE(job.Begin(request));
    Finish();
    EXPECT_EQ(pool.Count(), 1u);
    EXPECT_EQ(job.Status().sample_id, record->sample_id);
    EXPECT_EQ(job.BytesRead(), 0u);  // a resident hit does not allocate or read again
}

TEST_F(SampleLoadJobTest, BusyRequestCannotReplacePrivateAllocationOrRequestIdentity) {
    ReachPayload();
    Pump();
    auto other = request;
    other.sample_id = 456;
    std::strcpy(other.path, "/missing.wav");
    EXPECT_FALSE(job.Begin(other));
    EXPECT_EQ(job.Status().sample_id, request.sample_id);
    Finish();
    EXPECT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_COMPLETE);
    EXPECT_NE(pool.FindByPath(request.path), nullptr);
}

TEST_F(SampleLoadJobTest, ReadFailureTruncationAndCloseFailureNeverPublishPartialAudio) {
    auto& fs = MockFatFS::Instance();
    for (int failure = 0; failure < 3; ++failure) {
        fs.AddFile(request.path, wave);
        ReachPayload();
        Pump();
        if (failure == 0)
            fs.read_result = FR_DISK_ERR;
        else if (failure == 1)
            fs.MutableFile(request.path)->resize(44 + job.BytesRead() + 20);
        else
            fs.read_close_result = FR_DISK_ERR;
        Finish();
        EXPECT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_FAILED);
        EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_READ);
        EXPECT_EQ(job.Status().sample_id, request.sample_id);
        ExpectNoAllocation();
        job.ReplySent();
        fs.read_result = fs.read_close_result = FR_OK;
    }
    fs.AddFile(request.path, wave);
    ASSERT_TRUE(job.Begin(request));
    Finish();
    EXPECT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_COMPLETE);
}

TEST_F(SampleLoadJobTest, CancellationAtEveryPhaseReleasesPrivateStorage) {
    for (int passes = 0; passes < 10; ++passes) {
        ASSERT_TRUE(job.Begin(request));
        for (int i = 0; i < passes; ++i)
            Pump();
        ASSERT_TRUE(job.Busy());
        job.Cancel(memory);
        EXPECT_FALSE(job.Busy());
        EXPECT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_FAILED);
        ExpectNoAllocation();
        job.ReplySent();
    }
}

TEST_F(SampleLoadJobTest, SidecarEditsAreAppliedToThePublishedRecord) {
    SampleFile::Document geometry;
    ASSERT_TRUE(Storage::ProbeSampleFile(request.path, geometry));
    geometry.sample.start_frame = 50;
    geometry.sample.loop_start = 50;
    geometry.sample.gain_db_x10 = 120;
    Storage::SampleFileJob save;
    SampleFileOpMessage operation;
    operation.request_id = 1;
    operation.op = SAMPLE_FILE_SAVE;
    operation.sample_id = 123;
    ASSERT_TRUE(save.Begin(operation, request.path, geometry.sample));
    for (int i = 0; i < 100 && save.Busy(); ++i)
        save.Pump();
    ASSERT_EQ(save.Error(), SAMPLE_FILE_OK);
    ASSERT_TRUE(job.Begin(request));
    Finish();
    ASSERT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_COMPLETE);
    const auto* record = pool.Find(job.Status().sample_id);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->payload.meta.start_frame, 50u);
    EXPECT_EQ(record->payload.meta.gain_db_x10, 120);
}

TEST_F(SampleLoadJobTest, InvalidFileAndSidecarRejectBeforeAllocation) {
    for (const char fill: {'\0', 'x'}) {
        auto invalid_request = request;
        std::memset(invalid_request.path, fill, sizeof(invalid_request.path));
        ASSERT_TRUE(job.Begin(invalid_request));
        EXPECT_FALSE(job.Busy());
        EXPECT_TRUE(job.ReplyPending());
        EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_FORMAT);
        ExpectNoAllocation();
        job.ReplySent();
    }
    for (int failure = 0; failure < 3; ++failure) {
        auto invalid = wave;
        if (failure == 0)
            invalid[0] = 0;
        if (failure == 1)
            invalid[34] = 24;
        MockFatFS::Instance().AddFile(request.path, invalid);
        if (failure == 2)
            MockFatFS::Instance().AddFile("/source.wav.wxs", {1, 2, 3});
        ASSERT_TRUE(job.Begin(request));
        Finish();
        EXPECT_EQ(job.Status().state, SAMPLE_STATUS_LOAD_FAILED);
        EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_FORMAT);
        ExpectNoAllocation();
        job.ReplySent();
    }
}

TEST_F(SampleLoadJobTest, InsufficientMemoryAndUnusableScratchFailWithoutLeaking) {
    wxsamp_t occupied{};
    ASSERT_TRUE(memory.alloc(448 * 1024, &occupied));
    ASSERT_TRUE(job.Begin(request));
    Finish();
    EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_RAM);
    memory.release(&occupied);
    ExpectNoAllocation();
    job.ReplySent();
    ReachPayload();
    job.Pump(pool, memory, io.data() + 1, static_cast<uint32_t>(io.size() - 1));
    EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_READ);
    ExpectNoAllocation();
}

TEST_F(SampleLoadJobTest, FullPoolAndMissingPathRejectWithoutEviction) {
    std::strcpy(request.path, "/missing.wav");
    ASSERT_TRUE(job.Begin(request));
    Finish();
    EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_OPEN);
    ExpectNoAllocation();
    job.ReplySent();
    for (size_t i = 0; i < SamplePool::kCapacity; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/resident-%u.wav", static_cast<unsigned>(i));
        SamplePool::Record* record = nullptr;
        ASSERT_EQ(pool.AdmitPath(path, &record), SamplePool::Admit::Ok);
    }
    std::strcpy(request.path, "/source.wav");
    ASSERT_TRUE(job.Begin(request));
    Finish();
    EXPECT_EQ(job.Status().frames_played, SAMPLE_LOAD_FAIL_REGISTRY_FULL);
    EXPECT_EQ(pool.Count(), SamplePool::kCapacity);
    EXPECT_EQ(pool.FindByPath(request.path), nullptr);
}
}  // namespace
