#include "storage/sample_file_job.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

namespace {
using namespace WaveX;
using namespace WaveX::Protocol;
class SampleFileJobTest : public ::testing::Test {
   protected:
    Storage::SampleFileJob job;
    SampleFile::Document geometry;
    void SetUp() override {
        auto& fs = MockFatFS::Instance();
        fs.Reset();
        std::vector<uint8_t> wave(44 + 20000);
        std::memcpy(wave.data(), "RIFF", 4);
        std::memcpy(wave.data() + 8, "WAVEfmt ", 8);
        Wxcf::detail::WriteU32LE(wave.data() + 4, static_cast<uint32_t>(wave.size() - 8));
        Wxcf::detail::WriteU32LE(wave.data() + 16, 16);
        Wxcf::detail::WriteU16LE(wave.data() + 20, 1);
        Wxcf::detail::WriteU16LE(wave.data() + 22, 2);
        Wxcf::detail::WriteU32LE(wave.data() + 24, 48000);
        Wxcf::detail::WriteU32LE(wave.data() + 28, 192000);
        Wxcf::detail::WriteU16LE(wave.data() + 32, 4);
        Wxcf::detail::WriteU16LE(wave.data() + 34, 16);
        std::memcpy(wave.data() + 36, "data", 4);
        Wxcf::detail::WriteU32LE(wave.data() + 40, 20000);
        for (size_t i = 44; i < wave.size(); ++i)
            wave[i] = static_cast<uint8_t>(i);
        fs.AddFile("/samples/source.wav", wave);
        ASSERT_TRUE(Storage::ProbeSampleFile("/samples/source.wav", geometry));
        geometry.sample.sample_id = 1234;
        geometry.sample.start_frame = 50;
        geometry.sample.loop_start = 70;
        geometry.sample.loop_end = 4000;
        geometry.sample.end_frame = 4500;
        geometry.sample.gain_db_x10 = -75;
    }
    void TearDown() override { job.Cancel(); }
    void Begin(uint8_t op = SAMPLE_FILE_SAVE, uint32_t id = 1) {
        SampleFileOpMessage request;
        request.request_id = id;
        request.op = op;
        request.sample_id = 1234;
        std::strcpy(request.name, "Copy");
        ASSERT_TRUE(job.Begin(request, "/samples/source.wav", geometry.sample));
    }
    uint8_t Complete() {
        for (unsigned i = 0; i < 100 && job.Busy(); ++i)
            job.Pump();
        EXPECT_FALSE(job.Busy());
        return job.Error();
    }
};
TEST_F(SampleFileJobTest, SaveRestoreAndBackupRecovery) {
    Begin();
    ASSERT_EQ(Complete(), SAMPLE_FILE_OK);
    auto restored = geometry.sample;
    restored.start_frame = 0;
    ASSERT_EQ(Storage::ReadSampleSidecar("/samples/source.wav", geometry, restored),
              Storage::SampleSidecarResult::Loaded);
    EXPECT_EQ(restored.start_frame, 50u);
    EXPECT_EQ(restored.gain_db_x10, -75);
    geometry.sample.start_frame = 60;
    Begin(SAMPLE_FILE_SAVE, 2);
    ASSERT_EQ(Complete(), SAMPLE_FILE_OK);
    auto& fs = MockFatFS::Instance();
    ASSERT_NE(fs.GetFile("/samples/source.wav.wxs.bak"), nullptr);
    fs.RemoveFile("/samples/source.wav.wxs");  // reset between replacement renames
    ASSERT_EQ(Storage::ReadSampleSidecar("/samples/source.wav", geometry, restored),
              Storage::SampleSidecarResult::Loaded);
    EXPECT_EQ(restored.start_frame, 50u);
    Begin(SAMPLE_FILE_SAVE, 3);
    EXPECT_EQ(Complete(), SAMPLE_FILE_OK);
}
TEST_F(SampleFileJobTest, CopyYieldsAndPublishesIndependentAudioAndSidecar) {
    Begin(SAMPLE_FILE_COPY);
    job.Pump();
    job.Pump();
    job.Pump();
    ASSERT_TRUE(job.Busy());
    auto& fs = MockFatFS::Instance();
    EXPECT_EQ(fs.GetFile("/samples/Copy.wav"), nullptr);
    ASSERT_EQ(Complete(), SAMPLE_FILE_OK);
    ASSERT_NE(fs.GetFile("/samples/Copy.wav"), nullptr);
    EXPECT_EQ(*fs.GetFile("/samples/Copy.wav"), *fs.GetFile("/samples/source.wav"));
    SampleMetadata restored = geometry.sample;
    ASSERT_EQ(Storage::ReadSampleSidecar("/samples/Copy.wav", geometry, restored),
              Storage::SampleSidecarResult::Loaded);
    EXPECT_EQ(restored.start_frame, 50u);
    Begin(SAMPLE_FILE_COPY, 2);
    EXPECT_EQ(Complete(), SAMPLE_FILE_EXISTS);
}
TEST_F(SampleFileJobTest, WriteCloseRenameAndSpaceFailuresPreserveOldSave) {
    Begin();
    ASSERT_EQ(Complete(), SAMPLE_FILE_OK);
    auto& fs = MockFatFS::Instance();
    const auto saved = *fs.GetFile("/samples/source.wav.wxs");
    for (unsigned failure = 0; failure < 4; ++failure) {
        if (failure == 0)
            fs.write_limit = 10;
        if (failure == 1)
            fs.close_result = FR_DISK_ERR;
        if (failure == 2)
            fs.rename_result = FR_DISK_ERR;
        if (failure == 3)
            fs.free_clusters = 0;
        Begin(SAMPLE_FILE_SAVE, failure + 2);
        EXPECT_NE(Complete(), SAMPLE_FILE_OK);
        ASSERT_NE(fs.GetFile("/samples/source.wav.wxs"), nullptr);
        EXPECT_EQ(*fs.GetFile("/samples/source.wav.wxs"), saved);
        fs.write_limit = -1;
        fs.close_result = fs.rename_result = FR_OK;
        fs.free_clusters = 100000;
    }
}
TEST_F(SampleFileJobTest, CancellationAndUnownedTemporaryDoNotPublishOrDeleteOtherFiles) {
    auto& fs = MockFatFS::Instance();
    fs.AddFile("/samples/source.wav.wxs-00000001.tmp", {7});
    Begin();
    EXPECT_EQ(Complete(), SAMPLE_FILE_EXISTS);
    EXPECT_EQ(*fs.GetFile("/samples/source.wav.wxs-00000001.tmp"), std::vector<uint8_t>{7});
    Begin(SAMPLE_FILE_COPY, 2);
    job.Pump();
    job.Pump();
    job.Pump();
    job.Cancel();
    EXPECT_EQ(fs.GetFile("/samples/Copy.wav"), nullptr);
    EXPECT_EQ(fs.GetFile("/samples/Copy.wav-00000002.tmp"), nullptr);
}
TEST_F(SampleFileJobTest, ChangedWavAndMalformedSidecarAreRejected) {
    Begin();
    ASSERT_EQ(Complete(), SAMPLE_FILE_OK);
    auto& fs = MockFatFS::Instance();
    fs.MutableFile("/samples/source.wav.wxs")->pop_back();
    auto restored = geometry.sample;
    EXPECT_EQ(Storage::ReadSampleSidecar("/samples/source.wav", geometry, restored),
              Storage::SampleSidecarResult::Invalid);
    EXPECT_EQ(restored.start_frame, 50u);
    ++geometry.sample.sample_rate;
    Begin(SAMPLE_FILE_SAVE, 2);
    EXPECT_EQ(Complete(), SAMPLE_FILE_CHANGED);
}
}  // namespace
