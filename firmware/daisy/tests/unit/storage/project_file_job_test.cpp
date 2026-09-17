#include "storage/project_file_job.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include <memory>

namespace {
using WaveX::Storage::ProjectFileJob;
using Result = ProjectFileJob::Result;
class ProjectFileJobTest : public ::testing::Test {
   protected:
    std::unique_ptr<WaveX::Sequencer::Project> project;
    ProjectFileJob job;
    void SetUp() override {
        MockFatFS::Instance().Reset();
        project = std::make_unique<WaveX::Sequencer::Project>();
        std::strcpy(project->name, "Session");
        project->active_pattern = 127;
        project->patterns[127].used = true;
        std::strcpy(project->patterns[127].name, "Ending");
        project->patterns[127].pattern.length = 4;
        project->patterns[127].pattern.tracks[15].steps[63].note = 91;
        project->tracks[3].mix.gain = .25f;
        project->tracks[3].mix.mute = true;
        project->tempo_bpm_x100 = 14250;
    }
    void TearDown() override { job.Cancel(); }
    Result Complete() {
        for (unsigned i = 0; i < 40000 && job.Busy(); ++i)
            job.Pump();
        EXPECT_FALSE(job.Busy());
        return job.Status();
    }
    void Save() {
        ASSERT_TRUE(job.SaveCopy(*project, 1));
        ASSERT_EQ(Complete(), Result::Saved);
    }
};
TEST_F(ProjectFileJobTest, RoundTripKeepsSparseSlotsHiddenStepsAndTrackMix) {
    ASSERT_TRUE(job.SaveCopy(*project, 1));
    job.Pump();
    EXPECT_TRUE(job.Busy());
    EXPECT_EQ(MockFatFS::Instance().GetFile("0:/wavex/projects/Session.wxp"), nullptr);
    EXPECT_EQ(Complete(), Result::Saved);
    const auto* saved = MockFatFS::Instance().GetFile("0:/wavex/projects/Session.wxp");
    ASSERT_NE(saved, nullptr);
    EXPECT_EQ(job.FileBytes(), saved->size());
    auto scratch = std::make_unique<WaveX::Sequencer::Project>();
    ASSERT_TRUE(job.Load("Session", *scratch));
    EXPECT_EQ(Complete(), Result::Loaded);
    EXPECT_EQ(scratch->active_pattern, 127);
    EXPECT_FALSE(scratch->patterns[0].used);
    EXPECT_TRUE(scratch->patterns[127].used);
    EXPECT_EQ(scratch->patterns[127].pattern.tracks[15].steps[63].note, 91);
    EXPECT_FLOAT_EQ(scratch->tracks[3].mix.gain, .25f);
    EXPECT_TRUE(scratch->tracks[3].mix.mute);
    EXPECT_EQ(scratch->tempo_bpm_x100, 14250);
}
TEST_F(ProjectFileJobTest, RejectsInvalidInputAndCapacityBeforeCreatingFiles) {
    auto& fs = MockFatFS::Instance();
    project->patterns[127].pattern.tracks[15].steps[63].note = 200;
    ASSERT_TRUE(job.SaveCopy(*project, 1));
    EXPECT_EQ(Complete(), Result::Invalid);
    EXPECT_EQ(fs.GetDirectory("0:/wavex"), nullptr);
    project->patterns[127].pattern.tracks[15].steps[63].note = 91;
    fs.free_clusters = 0;
    ASSERT_TRUE(job.SaveCopy(*project, 2));
    EXPECT_EQ(Complete(), Result::NoSpace);
    EXPECT_EQ(fs.GetDirectory("0:/wavex"), nullptr);
    fs.free_result = FR_DISK_ERR;
    ASSERT_TRUE(job.SaveCopy(*project, 3));
    EXPECT_EQ(Complete(), Result::IoError);
    EXPECT_EQ(fs.GetDirectory("0:/wavex"), nullptr);
    EXPECT_FALSE(job.Load("../escape", *project));
    EXPECT_EQ(job.Status(), Result::BadName);
}
TEST_F(ProjectFileJobTest, ExistingDestinationAndUnownedTemporaryRemainUntouched) {
    Save();
    auto& fs = MockFatFS::Instance();
    const auto original = *fs.GetFile("0:/wavex/projects/Session.wxp");
    project->tempo_bpm_x100 = 9000;
    ASSERT_TRUE(job.SaveCopy(*project, 2));
    EXPECT_EQ(Complete(), Result::Exists);
    EXPECT_EQ(*fs.GetFile("0:/wavex/projects/Session.wxp"), original);
    std::strcpy(project->name, "Other");
    fs.AddFile("0:/wavex/projects/.Other-00000002.tmp", {1, 2, 3});
    ASSERT_TRUE(job.SaveCopy(*project, 2));
    EXPECT_EQ(Complete(), Result::Exists);
    ASSERT_NE(fs.GetFile("0:/wavex/projects/.Other-00000002.tmp"), nullptr);
    EXPECT_EQ(fs.GetFile("0:/wavex/projects/.Other-00000002.tmp")->size(), 3u);
}
TEST_F(ProjectFileJobTest, ShortWriteCloseAndRenameFailuresNeverPublish) {
    for (int failure = 0; failure < 3; ++failure) {
        auto& fs = MockFatFS::Instance();
        fs.Reset();
        if (failure == 0)
            fs.write_limit = 100;
        if (failure == 1)
            fs.close_result = FR_DISK_ERR;
        if (failure == 2)
            fs.rename_result = FR_DISK_ERR;
        ASSERT_TRUE(job.SaveCopy(*project, 1));
        EXPECT_EQ(Complete(), Result::IoError);
        EXPECT_EQ(fs.GetFile("0:/wavex/projects/Session.wxp"), nullptr);
        EXPECT_EQ(fs.GetFile("0:/wavex/projects/.Session-00000001.tmp"), nullptr);
    }
}
TEST_F(ProjectFileJobTest, BusyRequestCannotReplaceJobAndCancelRemovesOnlyOwnedTemp) {
    ASSERT_TRUE(job.SaveCopy(*project, 1));
    EXPECT_FALSE(job.Load("Elsewhere", *project));
    auto& fs = MockFatFS::Instance();
    for (int i = 0; i < 2000 && !fs.GetFile("0:/wavex/projects/.Session-00000001.tmp"); ++i)
        job.Pump();
    ASSERT_NE(fs.GetFile("0:/wavex/projects/.Session-00000001.tmp"), nullptr);
    job.Pump();
    job.Cancel();
    EXPECT_EQ(job.Status(), Result::Cancelled);
    EXPECT_EQ(fs.GetFile("0:/wavex/projects/.Session-00000001.tmp"), nullptr);
    EXPECT_EQ(fs.GetFile("0:/wavex/projects/Session.wxp"), nullptr);
    EXPECT_TRUE(job.SaveCopy(*project, 2));
    EXPECT_EQ(Complete(), Result::Saved);
}
TEST_F(ProjectFileJobTest, MissingTruncatedAndCloseFailedLoadsAreNotSuccessful) {
    auto scratch = std::make_unique<WaveX::Sequencer::Project>();
    ASSERT_TRUE(job.Load("Missing", *scratch));
    EXPECT_EQ(Complete(), Result::NotFound);
    Save();
    auto& fs = MockFatFS::Instance();
    fs.read_close_result = FR_DISK_ERR;
    ASSERT_TRUE(job.Load("Session", *scratch));
    EXPECT_EQ(Complete(), Result::IoError);
    fs.read_close_result = FR_OK;
    fs.read_result = FR_DISK_ERR;
    ASSERT_TRUE(job.Load("Session", *scratch));
    EXPECT_EQ(Complete(), Result::IoError);
    fs.read_result = FR_OK;
    fs.MutableFile("0:/wavex/projects/Session.wxp")->pop_back();
    ASSERT_TRUE(job.Load("Session", *scratch));
    EXPECT_EQ(Complete(), Result::Invalid);
    // The live Project was never handed to the decoder.
    EXPECT_EQ(project->patterns[127].pattern.tracks[15].steps[63].note, 91);
}
}  // namespace
