#include "storage/recording_save.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include "storage/sample_file_job.hpp"
#include <vector>

using namespace WaveX;
using namespace WaveX::Protocol;
namespace {
struct RecordingSaveTest : testing::Test {
    Storage::RecordingSave job;
    std::vector<int16_t> pcm = std::vector<int16_t>(6000, 0);
    void SetUp() override { MockFatFS::Instance().Reset(); }
    void Finish() {
        for (unsigned i = 0; i < 100 && job.Busy(); ++i)
            job.Pump();
        ASSERT_FALSE(job.Busy());
    }
};
TEST_F(RecordingSaveTest, WritesStereoPcmAndPaddedNondestructiveTrim) {
    pcm[2000] = 1000;
    pcm[4001] = -1000;
    ASSERT_TRUE(job.Begin(1, "Stereo take", pcm.data(), 3000, 2));
    job.Pump();
    EXPECT_TRUE(job.Busy());
    EXPECT_EQ(MockFatFS::Instance().GetFile(job.Path()), nullptr);
    Finish();
    ASSERT_EQ(job.Error(), REC_OK);
    auto* wave = MockFatFS::Instance().GetFile(job.Path());
    ASSERT_NE(wave, nullptr);
    ASSERT_EQ(wave->size(), 44u + pcm.size() * 2);
    EXPECT_EQ(std::memcmp(wave->data() + 44, pcm.data(), pcm.size() * 2), 0);
    SampleFile::Document geometry;
    ASSERT_TRUE(Storage::ProbeSampleFile(job.Path(), geometry));
    EXPECT_EQ(geometry.sample.total_frames, 3000u);
    auto metadata = geometry.sample;
    ASSERT_EQ(Storage::ReadSampleSidecar(job.Path(), geometry, metadata),
              Storage::SampleSidecarResult::Loaded);
    EXPECT_EQ(metadata.start_frame, 520u);
    EXPECT_EQ(metadata.end_frame, 2481u);
    EXPECT_EQ(geometry.sample.channels, 2u);
    ASSERT_TRUE(job.Begin(2, "Stereo take", pcm.data(), 3000, 2));
    Finish();
    EXPECT_EQ(job.Error(), REC_EXISTS);
}
TEST_F(RecordingSaveTest, LongUserNameUsesShortExclusiveTransactionFiles) {
    const char* name = "HIL take 1790011998501251523 0";
    ASSERT_TRUE(job.Begin(0x1234, name, pcm.data(), 3000, 2));
    job.Pump();
    ASSERT_NE(MockFatFS::Instance().GetFile("/wavex/recordings/00001234.tmp"), nullptr);
    Finish();
    ASSERT_EQ(job.Error(), REC_OK);
    ASSERT_NE(
        MockFatFS::Instance().GetFile("/wavex/recordings/HIL take 1790011998501251523 0.wav.wxs"),
        nullptr);
    EXPECT_EQ(MockFatFS::Instance().GetFile("/wavex/recordings/00001234.tmp"), nullptr);
    EXPECT_EQ(MockFatFS::Instance().GetFile("/wavex/recordings/00001234.met"), nullptr);
}
TEST_F(RecordingSaveTest, SavesSilentOddMonoLengthWithoutTrimmingItAway) {
    ASSERT_TRUE(job.Begin(1, "Mono", pcm.data(), 3001, 1));
    Finish();
    ASSERT_EQ(job.Error(), REC_OK);
    EXPECT_EQ(job.Metadata().start_frame, 0u);
    EXPECT_EQ(job.Metadata().end_frame, 3001u);
    EXPECT_EQ(MockFatFS::Instance().GetFile(job.Path())->size(), 6046u);
}
TEST_F(RecordingSaveTest, RejectsInvalidNameAndNoCapture) {
    EXPECT_FALSE(job.Begin(1, "../bad", pcm.data(), 1, 1));
    EXPECT_EQ(job.Error(), REC_BAD_NAME);
    EXPECT_FALSE(job.Begin(1, "take", pcm.data(), 0, 1));
    EXPECT_EQ(job.Error(), REC_BAD_STATE);
}
TEST_F(RecordingSaveTest, WriteFailureLeavesTakeAndNoPublishedFile) {
    pcm[0] = 123;
    ASSERT_TRUE(job.Begin(1, "Retry", pcm.data(), 3000, 2));
    job.Pump();
    MockFatFS::Instance().write_limit = 10;
    Finish();
    EXPECT_EQ(job.Error(), REC_IO);
    EXPECT_EQ(MockFatFS::Instance().GetFile(job.Path()), nullptr);
    EXPECT_EQ(pcm[0], 123);
    MockFatFS::Instance().write_limit = -1;
    ASSERT_TRUE(job.Begin(2, "Retry", pcm.data(), 3000, 2));
    Finish();
    EXPECT_EQ(job.Error(), REC_OK);
}
}  // namespace
