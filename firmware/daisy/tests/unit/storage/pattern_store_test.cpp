#include "storage/pattern_store.hpp"

#include <gtest/gtest.h>

#include "fatfs_mock.h"

#include <cstring>
namespace WaveX::Comm {
static WaveX::Protocol::SeqFileStatusMessage last;
static bool drop = false;
int UartLinkSend(uint16_t type, const void* payload, uint16_t length) {
    if (drop)
        return -1;
    if (type == WaveX::Protocol::MSG_SEQ_FILE_STATUS && length == sizeof(last))
        std::memcpy(&last, payload, length);
    return length;
}
}  // namespace WaveX::Comm
namespace {
using namespace WaveX;
using namespace Protocol;
class PatternStoreTest : public ::testing::Test {
   protected:
    Sequencer::SequencerTransport transport;
    Sequencer::PatternExchange exchange;
    static uint32_t counter;
    void SetUp() override {
        MockFatFS::Instance().Reset();
        Comm::drop = false;
        transport.Init(48000, 48);
    }
    SeqFileOpMessage Request(uint8_t op, const char* name = "") {
        SeqFileOpMessage request;
        request.request_id = ++counter;
        request.op = op;
        detail::CopyWireString(request.name, sizeof(request.name), name);
        PatternStore::Request(request, exchange);
        return request;
    }
    SeqFileStatusMessage Complete() {
        for (int i = 0; i < 2000; ++i) {
            exchange.Process(transport);
            PatternStore::Pump(exchange);
            if (!Comm::last.busy && !Comm::drop)
                return Comm::last;
        }
        ADD_FAILURE() << "pattern job did not finish";
        return Comm::last;
    }
    void Save(const char* name) {
        Request(SEQ_FILE_SAVE_COPY, name);
        ASSERT_EQ(Complete().error, SEQ_FILE_OK);
    }
};
uint32_t PatternStoreTest::counter = 1000;
TEST_F(PatternStoreTest, SaveChecksFreeSpaceBeforeCreatingAnyFile) {
    auto& fs = MockFatFS::Instance();
    fs.free_clusters = 0;
    Request(SEQ_FILE_SAVE_COPY, "Full");
    EXPECT_EQ(Complete().error, SEQ_FILE_NO_SPACE);
    EXPECT_EQ(fs.GetFile("0:/wavex/patterns/Full.wxpat"), nullptr);
    fs.free_result = FR_DISK_ERR;
    Request(SEQ_FILE_SAVE_COPY, "Unknown");
    EXPECT_EQ(Complete().error, SEQ_FILE_IO);
    EXPECT_EQ(fs.GetFile("0:/wavex/patterns/Unknown.wxpat"), nullptr);
}
TEST_F(PatternStoreTest, SaveLoadRoundTripPreservesTempoAndStopsPlayback) {
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 15, 63, 127, 0, 0});
    transport.ApplyPatternOp({SEQ_OP_SET_STEP, 15, 63, 1, 77, 0});
    transport.ApplyPatternOp({SEQ_OP_PATTERN_SWING, 0, 0, 71, 0, 0});
    Save("Midnight");
    ASSERT_NE(MockFatFS::Instance().GetFile("0:/wavex/patterns/Midnight.wxpat"), nullptr);
    Request(SEQ_FILE_NEW);
    ASSERT_EQ(Complete().error, SEQ_FILE_OK);
    EXPECT_EQ(transport.pattern().tracks[15].steps[63].note, 60);
    transport.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 14700, 0});
    Request(SEQ_FILE_LOAD, "Midnight");
    EXPECT_TRUE(PatternStore::BlocksEdits());
    ASSERT_EQ(Complete().error, SEQ_FILE_OK);
    EXPECT_FALSE(PatternStore::BlocksEdits());
    EXPECT_FALSE(transport.IsPlaying());
    EXPECT_DOUBLE_EQ(transport.TempoBpm(), 147);
    EXPECT_EQ(transport.pattern().tracks[15].steps[63].note, 127);
    EXPECT_EQ(transport.pattern().tracks[15].steps[63].velocity, 77);
    EXPECT_EQ(transport.pattern().swing, 71);
}
TEST_F(PatternStoreTest, FailedAndDuplicateSavesPreserveEarlierFile) {
    Save("Earlier");
    const auto earlier = *MockFatFS::Instance().GetFile("0:/wavex/patterns/Earlier.wxpat");
    Request(SEQ_FILE_SAVE_COPY, "Earlier");
    EXPECT_EQ(Complete().error, SEQ_FILE_EXISTS);
    EXPECT_EQ(*MockFatFS::Instance().GetFile("0:/wavex/patterns/Earlier.wxpat"), earlier);
    for (int failure = 0; failure < 3; ++failure) {
        auto& fs = MockFatFS::Instance();
        if (failure == 0)
            fs.write_limit = 100;
        if (failure == 1)
            fs.close_result = FR_DISK_ERR;
        if (failure == 2)
            fs.rename_result = FR_DISK_ERR;
        auto request = Request(SEQ_FILE_SAVE_COPY, "Failed");
        EXPECT_EQ(Complete().error, SEQ_FILE_IO);
        EXPECT_EQ(fs.GetFile("0:/wavex/patterns/Failed.wxpat"), nullptr);
        char temporary[112];
        std::snprintf(temporary,
                      sizeof(temporary),
                      "0:/wavex/patterns/.Failed-%08lx.tmp",
                      static_cast<unsigned long>(request.request_id));
        EXPECT_EQ(fs.GetFile(temporary), nullptr);
        EXPECT_EQ(*fs.GetFile("0:/wavex/patterns/Earlier.wxpat"), earlier);
        fs.write_limit = -1;
        fs.close_result = fs.rename_result = FR_OK;
    }
}
TEST_F(PatternStoreTest, InvalidAndMissingFilesNeverReplaceWorkingPattern) {
    transport.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 0, 0, 75, 0, 0});
    Request(SEQ_FILE_LOAD, "Missing");
    EXPECT_EQ(Complete().error, SEQ_FILE_NOT_FOUND);
    EXPECT_EQ(transport.pattern().tracks[0].steps[0].note, 75);
    Save("Broken");
    auto* bytes = MockFatFS::Instance().MutableFile("0:/wavex/patterns/Broken.wxpat");
    ASSERT_NE(bytes, nullptr);
    bytes->pop_back();
    Request(SEQ_FILE_LOAD, "Broken");
    EXPECT_EQ(Complete().error, SEQ_FILE_BAD_FILE);
    EXPECT_EQ(transport.pattern().tracks[0].steps[0].note, 75);
}
TEST_F(PatternStoreTest, ReadRetryRetainsCompletionAndDuplicateDoesNotReplay) {
    Comm::drop = true;
    const auto request = Request(SEQ_FILE_SAVE_COPY, "Once");
    for (int i = 0; i < 2000; ++i) {
        exchange.Process(transport);
        PatternStore::Pump(exchange);
    }
    Comm::drop = false;
    const auto read = Request(SEQ_FILE_GET);
    PatternStore::Pump(exchange);
    EXPECT_EQ(Comm::last.request_id, read.request_id);
    EXPECT_EQ(Comm::last.completed_request_id, request.request_id);
    EXPECT_EQ(Comm::last.error, SEQ_FILE_OK);
    PatternStore::Request(request, exchange);
    PatternStore::Pump(exchange);
    EXPECT_FALSE(Comm::last.busy);
    EXPECT_EQ(Comm::last.error, SEQ_FILE_OK);
}
TEST_F(PatternStoreTest, BusyRequestCannotReplaceActiveJobOrItsCompletion) {
    const auto saving = Request(SEQ_FILE_SAVE_COPY, "Busy");
    const auto rejected = Request(SEQ_FILE_NEW);
    PatternStore::Pump(exchange);
    EXPECT_EQ(Comm::last.completed_request_id, rejected.request_id);
    EXPECT_EQ(Comm::last.error, SEQ_FILE_BUSY);
    EXPECT_TRUE(Comm::last.busy);
    EXPECT_EQ(Complete().completed_request_id, saving.request_id);
    EXPECT_EQ(Comm::last.error, SEQ_FILE_OK);
}
TEST_F(PatternStoreTest, RejectsUnsafeNamesAndIgnoresUnusedNewName) {
    Request(SEQ_FILE_SAVE_COPY, "../escape");
    EXPECT_EQ(Complete().error, SEQ_FILE_BAD_NAME);
    SeqFileOpMessage request;
    request.request_id = ++counter;
    request.op = SEQ_FILE_NEW;
    std::memset(request.name, 'X', sizeof(request.name));
    PatternStore::Request(request, exchange);
    EXPECT_EQ(Complete().error, SEQ_FILE_OK);
}
}  // namespace

TEST_F(PatternStoreTest, ReadAndDuplicateRequestsCannotRepeatPreviewStopSideEffects) {
    SeqFileOpMessage request;
    request.request_id = ++counter;
    request.op = SEQ_FILE_SAVE_COPY;
    std::strcpy(request.name, "Preview");
    ASSERT_TRUE(PatternStore::Request(request, exchange));
    EXPECT_FALSE(PatternStore::Request(request, exchange));
    ASSERT_EQ(Complete().error, SEQ_FILE_OK);
    EXPECT_FALSE(PatternStore::Request(request, exchange));
    request.request_id = ++counter;
    request.op = SEQ_FILE_GET;
    EXPECT_FALSE(PatternStore::Request(request, exchange));
}
