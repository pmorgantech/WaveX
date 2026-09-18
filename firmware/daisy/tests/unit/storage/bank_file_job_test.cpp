#include "storage/bank_file_job.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include <memory>

namespace {
using namespace WaveX;
using Storage::BankFileJob;
using R = BankFileJob::Result;
class BankFileJobTest : public ::testing::Test {
   protected:
    BankFileJob job;
    std::unique_ptr<Wxi::InstrumentFile> doc;
    void SetUp() override {
        MockFatFS::Instance().Reset();
        doc = std::make_unique<Wxi::InstrumentFile>();
        std::strcpy(doc->name, "Keys");
        doc->tags = 2;
        doc->osc[0].zone_count = 1;
        std::strcpy(doc->osc[0].zones[0].path, "/keys.wav");
        doc->lfo[0].rate_hz = .01f;
        doc->lfo[1].rate_hz = 100.f;
    }
    void TearDown() override { job.Cancel(); }
    R Complete() {
        for (unsigned i = 0; i < 70000 && job.Busy(); ++i)
            job.Pump();
        EXPECT_FALSE(job.Busy());
        return job.Status();
    }
    void Save() {
        ASSERT_TRUE(job.SaveCopy("Base", 1, nullptr, 127, doc.get()));
        ASSERT_EQ(Complete(), R::Saved);
    }
};
TEST_F(BankFileJobTest, EmptyCreateSparseStoreCopyAndClearPreserveSlotIdentity) {
    auto& fs = MockFatFS::Instance();
    ASSERT_TRUE(job.SaveCopy("Empty", 1));
    ASSERT_EQ(Complete(), R::Saved);
    ASSERT_TRUE(job.LoadIndex("Empty"));
    ASSERT_EQ(Complete(), R::Indexed);
    for (const auto& slot: job.Index().slots)
        EXPECT_FALSE(slot.used());
    Save();
    const auto original = *fs.GetFile("0:/wavex/banks/Base.wxb");
    EXPECT_EQ(original.size(), job.FileBytes());
    ASSERT_TRUE(job.LoadIndex("Base"));
    ASSERT_EQ(Complete(), R::Indexed);
    // A caller naturally passes the loaded index name as the source.
    std::strcpy(doc->name, "Bass");
    ASSERT_TRUE(job.SaveCopy("Layered", 2, job.Index().name, 2, doc.get()));
    ASSERT_EQ(Complete(), R::Saved);
    auto read = std::make_unique<Wxi::InstrumentFile>();
    ASSERT_TRUE(job.ReadInstrument("Layered", 127, *read));
    ASSERT_EQ(Complete(), R::InstrumentRead);
    EXPECT_STREQ(read->name, "Keys");
    EXPECT_FLOAT_EQ(read->lfo[0].rate_hz, .01f);
    EXPECT_FLOAT_EQ(read->lfo[1].rate_hz, 100.f);
    EXPECT_TRUE(job.Index().slots[2].used());
    EXPECT_FALSE(job.Index().slots[3].used());
    ASSERT_TRUE(job.ReadInstrument("Layered", 2, *read));
    ASSERT_EQ(Complete(), R::InstrumentRead);
    EXPECT_STREQ(read->name, "Bass");
    ASSERT_TRUE(job.ReadInstrument("Layered", 3, *read));
    EXPECT_EQ(Complete(), R::EmptySlot);
    ASSERT_TRUE(job.SaveCopy("Cleared", 3, "Layered", 127));
    ASSERT_EQ(Complete(), R::Saved);
    ASSERT_TRUE(job.LoadIndex("Cleared"));
    ASSERT_EQ(Complete(), R::Indexed);
    EXPECT_TRUE(job.Index().slots[2].used());
    EXPECT_FALSE(job.Index().slots[127].used());
    EXPECT_EQ(*fs.GetFile("0:/wavex/banks/Base.wxb"), original);
}
TEST_F(BankFileJobTest, SpaceAndSpaceQueryFailureCreateNothing) {
    auto& fs = MockFatFS::Instance();
    fs.free_clusters = 0;
    ASSERT_TRUE(job.SaveCopy("Base", 1, nullptr, 0, doc.get()));
    EXPECT_EQ(Complete(), R::NoSpace);
    EXPECT_EQ(fs.GetDirectory("0:/wavex"), nullptr);
    fs.free_clusters = 100000;
    fs.free_result = FR_DISK_ERR;
    ASSERT_TRUE(job.SaveCopy("Base", 2));
    EXPECT_EQ(Complete(), R::IoError);
    EXPECT_EQ(fs.GetDirectory("0:/wavex"), nullptr);
}
TEST_F(BankFileJobTest, ExistingBankAndUnownedTemporaryArePreserved) {
    Save();
    auto& fs = MockFatFS::Instance();
    const auto original = *fs.GetFile("0:/wavex/banks/Base.wxb");
    ASSERT_TRUE(job.SaveCopy("Base", 2));
    EXPECT_EQ(Complete(), R::Exists);
    EXPECT_EQ(*fs.GetFile("0:/wavex/banks/Base.wxb"), original);
    fs.AddFile("0:/wavex/banks/.Other-00000002.tmp", {1, 2, 3});
    ASSERT_TRUE(job.SaveCopy("Other", 2));
    EXPECT_EQ(Complete(), R::Exists);
    ASSERT_NE(fs.GetFile("0:/wavex/banks/.Other-00000002.tmp"), nullptr);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/.Other-00000002.tmp")->size(), 3u);
}
TEST_F(BankFileJobTest, ShortWriteCloseAndRenameFailuresDoNotPublish) {
    auto& fs = MockFatFS::Instance();
    for (int failure = 0; failure < 3; ++failure) {
        fs.Reset();
        if (failure == 0)
            fs.write_limit = 100;
        if (failure == 1)
            fs.close_result = FR_DISK_ERR;
        if (failure == 2)
            fs.rename_result = FR_DISK_ERR;
        ASSERT_TRUE(job.SaveCopy("Failed", 1, nullptr, 0, doc.get()));
        EXPECT_EQ(Complete(), R::IoError);
        EXPECT_EQ(fs.GetFile("0:/wavex/banks/Failed.wxb"), nullptr);
        EXPECT_EQ(fs.GetFile("0:/wavex/banks/.Failed-00000001.tmp"), nullptr);
    }
}
TEST_F(BankFileJobTest, BusyCannotReplaceJobAndCancelKeepsSource) {
    Save();
    auto& fs = MockFatFS::Instance();
    const auto original = *fs.GetFile("0:/wavex/banks/Base.wxb");
    ASSERT_TRUE(job.SaveCopy("Other", 2, "Base"));
    EXPECT_FALSE(job.LoadIndex("Elsewhere"));
    EXPECT_FALSE(job.SaveCopy("Wrong", 0));
    for (unsigned i = 0; i < 70000 && !fs.GetFile("0:/wavex/banks/.Other-00000002.tmp"); ++i)
        job.Pump();
    ASSERT_NE(fs.GetFile("0:/wavex/banks/.Other-00000002.tmp"), nullptr);
    job.Pump();
    job.Cancel();
    EXPECT_EQ(job.Status(), R::Cancelled);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/.Other-00000002.tmp"), nullptr);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/Other.wxb"), nullptr);
    EXPECT_EQ(*fs.GetFile("0:/wavex/banks/Base.wxb"), original);
    ASSERT_TRUE(job.SaveCopy("Other", 3, "Base"));
    EXPECT_EQ(Complete(), R::Saved);
}
TEST_F(BankFileJobTest, MissingTruncatedDiskAndCloseFailuresAreNotSuccessfulLoads) {
    ASSERT_TRUE(job.LoadIndex("Missing"));
    EXPECT_EQ(Complete(), R::NotFound);
    Save();
    auto& fs = MockFatFS::Instance();
    fs.read_close_result = FR_DISK_ERR;
    ASSERT_TRUE(job.LoadIndex("Base"));
    EXPECT_EQ(Complete(), R::IoError);
    ASSERT_TRUE(job.SaveCopy("Other", 2, "Base"));
    EXPECT_EQ(Complete(), R::IoError);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/Other.wxb"), nullptr);
    fs.read_close_result = FR_OK;
    fs.read_result = FR_DISK_ERR;
    ASSERT_TRUE(job.LoadIndex("Base"));
    EXPECT_EQ(Complete(), R::IoError);
    fs.read_result = FR_OK;
    fs.MutableFile("0:/wavex/banks/Base.wxb")->pop_back();
    ASSERT_TRUE(job.LoadIndex("Base"));
    EXPECT_EQ(Complete(), R::Invalid);
    ASSERT_TRUE(job.SaveCopy("Other", 3, "Base"));
    EXPECT_EQ(Complete(), R::Invalid);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/.Other-00000003.tmp"), nullptr);
}
TEST_F(BankFileJobTest, RejectsUnsafeNamesAndInvalidOperations) {
    EXPECT_FALSE(job.LoadIndex("../escape"));
    EXPECT_EQ(job.Status(), R::BadName);
    EXPECT_FALSE(job.SaveCopy("Good", 1, "../escape"));
    EXPECT_EQ(job.Status(), R::BadName);
    EXPECT_FALSE(job.SaveCopy("Good", 0));
    EXPECT_EQ(job.Status(), R::Invalid);
    EXPECT_FALSE(job.SaveCopy("Good", 1, nullptr, 128, doc.get()));
    EXPECT_FALSE(job.ReadInstrument("Good", 128, *doc));
    std::strcpy(doc->name, "../escape");
    EXPECT_FALSE(job.SaveCopy("Good", 1, nullptr, 0, doc.get()));
    EXPECT_EQ(MockFatFS::Instance().GetDirectory("0:/wavex"), nullptr);
}

TEST_F(BankFileJobTest, SlotCopyMoveBothDirectionsPreserveDocumentBytesAndOriginal) {
    Save();
    auto& fs = MockFatFS::Instance();
    ASSERT_TRUE(job.LoadIndex("Base"));
    ASSERT_EQ(Complete(), R::Indexed);
    const auto initial = job.Index().slots[127];
    // A future embedded WXI extension must survive byte-for-byte. Re-encoding
    // the current Instrument model would silently lose it.
    auto& extended = *fs.MutableFile("0:/wavex/banks/Base.wxb");
    const uint8_t future[] = {0x00, 0x70, 0x00, 0x01, 3, 0, 0, 0, 11, 22, 33};
    extended.insert(extended.end(), std::begin(future), std::end(future));
    Wxcf::detail::WriteU32LE(extended.data() + initial.offset + 8, initial.bytes + sizeof(future));
    Wxcf::detail::WriteU32LE(extended.data() + initial.offset - BankFile::kSlotPrefixBytes - 4,
                             BankFile::kSlotPrefixBytes + initial.bytes + sizeof(future));
    const auto original = extended;
    ASSERT_TRUE(job.LoadIndex("Base"));
    ASSERT_EQ(Complete(), R::Indexed);
    const auto metadata = job.Index().slots[127];
    const std::vector<uint8_t> document(original.begin() + metadata.offset,
                                        original.begin() + metadata.offset + metadata.bytes);
    ASSERT_TRUE(job.TransferCopy("Copied", 2, job.Index().name, 127, 0, false));
    ASSERT_EQ(Complete(), R::Saved);
    EXPECT_EQ(job.FileBytes(), fs.GetFile("0:/wavex/banks/Copied.wxb")->size());
    ASSERT_TRUE(job.LoadIndex("Copied"));
    ASSERT_EQ(Complete(), R::Indexed);
    for (uint8_t slot: {uint8_t{0}, uint8_t{127}}) {
        const auto& entry = job.Index().slots[slot];
        ASSERT_TRUE(entry.used());
        EXPECT_STREQ(entry.name, "Keys");
        EXPECT_EQ(entry.tags, 2);
        const auto& file = *fs.GetFile("0:/wavex/banks/Copied.wxb");
        EXPECT_EQ(std::vector<uint8_t>(file.begin() + entry.offset,
                                       file.begin() + entry.offset + entry.bytes),
                  document);
    }
    // Replace an occupied destination, including when it follows the source.
    ASSERT_TRUE(job.TransferCopy("Moved", 3, "Copied", 0, 127, true));
    ASSERT_EQ(Complete(), R::Saved);
    ASSERT_TRUE(job.LoadIndex("Moved"));
    ASSERT_EQ(Complete(), R::Indexed);
    EXPECT_FALSE(job.Index().slots[0].used());
    EXPECT_TRUE(job.Index().slots[127].used());
    ASSERT_TRUE(job.TransferCopy("Reverse", 4, "Moved", 127, 0, true));
    ASSERT_EQ(Complete(), R::Saved);
    ASSERT_TRUE(job.ReadInstrument("Reverse", 0, *doc));
    ASSERT_EQ(Complete(), R::InstrumentRead);
    EXPECT_FLOAT_EQ(doc->lfo[0].rate_hz, .01f);
    EXPECT_FLOAT_EQ(doc->lfo[1].rate_hz, 100.f);
    EXPECT_FALSE(job.Index().slots[127].used());
    EXPECT_EQ(*fs.GetFile("0:/wavex/banks/Base.wxb"), original);
}
TEST_F(BankFileJobTest, TransfersRejectInvalidOrEmptySourceAndCheckSpace) {
    Save();
    auto& fs = MockFatFS::Instance();
    EXPECT_FALSE(job.TransferCopy("Other", 2, "Base", 127, 127, false));
    EXPECT_FALSE(job.TransferCopy("Other", 2, "Base", 128, 0, false));
    EXPECT_FALSE(job.TransferCopy("Other", 2, "Base", 0, 128, true));
    EXPECT_FALSE(job.TransferCopy("Other", 2, nullptr, 0, 1, true));
    ASSERT_TRUE(job.TransferCopy("Other", 2, "Base", 0, 1, true));
    EXPECT_EQ(Complete(), R::EmptySlot);
    fs.free_clusters = 0;
    ASSERT_TRUE(job.TransferCopy("Other", 3, "Base", 127, 0, false));
    EXPECT_EQ(Complete(), R::NoSpace);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/Other.wxb"), nullptr);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/.Other-00000003.tmp"), nullptr);
}
TEST_F(BankFileJobTest, TransferFailureAndCancelPreserveOriginal) {
    Save();
    auto& fs = MockFatFS::Instance();
    const auto original = *fs.GetFile("0:/wavex/banks/Base.wxb");
    fs.rename_result = FR_DISK_ERR;
    ASSERT_TRUE(job.TransferCopy("Other", 2, "Base", 127, 0, true));
    EXPECT_EQ(Complete(), R::IoError);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/Other.wxb"), nullptr);
    fs.rename_result = FR_OK;
    ASSERT_TRUE(job.TransferCopy("Other", 3, "Base", 127, 0, true));
    for (unsigned i = 0; i < 70000 && !fs.GetFile("0:/wavex/banks/.Other-00000003.tmp"); ++i)
        job.Pump();
    ASSERT_NE(fs.GetFile("0:/wavex/banks/.Other-00000003.tmp"), nullptr);
    job.Pump();
    job.Cancel();
    EXPECT_EQ(job.Status(), R::Cancelled);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/.Other-00000003.tmp"), nullptr);
    EXPECT_EQ(fs.GetFile("0:/wavex/banks/Other.wxb"), nullptr);
    EXPECT_EQ(*fs.GetFile("0:/wavex/banks/Base.wxb"), original);
}
}  // namespace
