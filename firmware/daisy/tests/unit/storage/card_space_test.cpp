#include "storage/card_space.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include "cv/cv_cal_store.hpp"
#include "storage/card_layout.hpp"
using namespace WaveX::Storage;
TEST(CardSpace, CreatesExpectedFoldersAndReportsDirectoryFailure) {
    auto& fs = MockFatFS::Instance();
    fs.Reset();
    ASSERT_TRUE(CreateCardDirectories());
    EXPECT_NE(fs.GetDirectory("0:/wavex/samples"), nullptr);
    EXPECT_NE(fs.GetDirectory("0:/wavex/sfz"), nullptr);
    EXPECT_NE(fs.GetDirectory("0:/wavex/instruments"), nullptr);
    EXPECT_NE(fs.GetDirectory("0:/wavex/banks"), nullptr);
    EXPECT_NE(fs.GetDirectory("0:/wavex/projects"), nullptr);
    EXPECT_NE(fs.GetDirectory("0:/wavex/recordings"), nullptr);
    EXPECT_NE(fs.GetDirectory("0:/wavex/patterns"), nullptr);
    EXPECT_TRUE(CreateCardDirectories());
    fs.mkdir_result = FR_DISK_ERR;
    EXPECT_FALSE(CreateCardDirectories());
    fs.Reset();
}
TEST(CardSpace, CountsRoundedClustersAndDirectoryHeadroomWithoutOverflow) {
    auto& fs = MockFatFS::Instance();
    fs.Reset();
    fs.cluster_sectors = 64;
    fs.free_clusters = 5;
    EXPECT_EQ(CheckSaveSpace(32768), SaveSpace::Ready);
    EXPECT_EQ(CheckSaveSpace(32769), SaveSpace::Full);
    fs.free_clusters = 131076;
    EXPECT_EQ(CheckSaveSpace(UINT32_MAX), SaveSpace::Ready);
    fs.free_clusters--;
    EXPECT_EQ(CheckSaveSpace(UINT32_MAX), SaveSpace::Full);
    fs.free_result = FR_DISK_ERR;
    EXPECT_EQ(CheckSaveSpace(1), SaveSpace::IoError);
    fs.Reset();
}
TEST(CardSpace, CalibrationChecksSpaceBeforeTruncatingAndChecksClose) {
    auto& fs = MockFatFS::Instance();
    fs.Reset();
    CvCal table[WAVEX_ANALOG_CV_GROUPS_MAX]{};
    FIL file{};
    ASSERT_TRUE(WaveX::Cv::SaveCvCalTable(file, table));
    const auto original = *fs.GetFile(WaveX::Cv::kCvCalPath);
    fs.free_clusters = 0;
    EXPECT_FALSE(WaveX::Cv::SaveCvCalTable(file, table));
    EXPECT_EQ(*fs.GetFile(WaveX::Cv::kCvCalPath), original);
    fs.free_result = FR_DISK_ERR;
    EXPECT_FALSE(WaveX::Cv::SaveCvCalTable(file, table));
    EXPECT_EQ(*fs.GetFile(WaveX::Cv::kCvCalPath), original);
    fs.Reset();
    fs.close_result = FR_DISK_ERR;
    EXPECT_FALSE(WaveX::Cv::SaveCvCalTable(file, table));
    fs.Reset();
}
