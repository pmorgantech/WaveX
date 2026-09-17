#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(PatternSlotProtocol, OperationsAndRetainedResultsRoundTrip) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op = SEQ_SLOT_GET; op <= SEQ_SLOT_LAUNCH; ++op) {
        SeqSlotOpMessage request;
        request.request_id = 0x81234567;
        request.op = op;
        request.slot = 127;
        std::strcpy(request.name, "Fill B");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SEQ_SLOT_OP, &request, sizeof(request)),
                  0u);
        SeqSlotOpMessage parsed;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_SLOT_OP, &parsed, sizeof(parsed)));
        EXPECT_EQ(std::memcmp(&request, &parsed, sizeof(parsed)), 0);
        EXPECT_TRUE(IsValidSeqSlotOp(parsed));
    }
    for (uint8_t error = SEQ_SLOT_OK; error <= SEQ_SLOT_CANCELLED; ++error) {
        SeqSlotStatusMessage status;
        status.request_id = 23;
        status.active_request_id = 45;
        status.completed_request_id = 67;
        status.busy = 1;
        status.error = error;
        status.completed_op = SEQ_SLOT_COPY;
        status.slot = 127;
        status.used = 1;
        status.active_pattern = 91;
        status.queued_pattern = 14;
        std::strcpy(status.name, "Verse A");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SEQ_SLOT_STATUS, &status, sizeof(status)),
                  0u);
        SeqSlotStatusMessage parsed;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            packet.data(), MSG_SEQ_SLOT_STATUS, &parsed, sizeof(parsed)));
        EXPECT_EQ(std::memcmp(&status, &parsed, sizeof(parsed)), 0);
        EXPECT_TRUE(IsValidSeqSlotStatus(parsed));
    }
}
TEST(PatternSlotProtocol, RejectsInvalidBoundsAndUnterminatedReplies) {
    SeqSlotOpMessage request;
    EXPECT_FALSE(IsValidSeqSlotOp(request));
    request.request_id = 1;
    request.slot = 128;
    EXPECT_FALSE(IsValidSeqSlotOp(request));
    request.slot = 127;
    request.reserved = 1;
    EXPECT_FALSE(IsValidSeqSlotOp(request));
    request.reserved = 0;
    request.op = SEQ_SLOT_LAUNCH + 1;
    EXPECT_FALSE(IsValidSeqSlotOp(request));
    SeqSlotStatusMessage status;
    status.request_id = 1;
    EXPECT_TRUE(IsValidSeqSlotStatus(status));
    status.busy = 1;
    EXPECT_FALSE(IsValidSeqSlotStatus(status));
    status.active_request_id = 1;
    status.active_pattern = 128;
    EXPECT_FALSE(IsValidSeqSlotStatus(status));
    status.active_pattern = 127;
    status.used = 2;
    EXPECT_FALSE(IsValidSeqSlotStatus(status));
    status.used = 1;
    std::memset(status.name, 'x', sizeof(status.name));
    EXPECT_FALSE(IsValidSeqSlotStatus(status));
}

TEST(PatternSlotProtocol, ScopedPageAndEditKeepIdentityAndRejectInvalidBounds) {
    std::array<uint8_t, 512> packet{};
    SeqSlotEditMessage edit;
    edit.epoch = 0x87654321;
    edit.pattern = 127;
    edit.edit = {SEQ_OP_SET_STEP_NOTE, 15, 63, 99, 321, -7};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SEQ_SLOT_EDIT, &edit, sizeof(edit)),
              0u);
    SeqSlotEditMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_SLOT_EDIT, &parsed, sizeof(parsed)));
    EXPECT_EQ(std::memcmp(&edit, &parsed, sizeof(edit)), 0);
    EXPECT_TRUE(IsValidSeqSlotEdit(parsed));
    parsed.epoch = 0;
    EXPECT_FALSE(IsValidSeqSlotEdit(parsed));
    SeqSlotPageMessage page;
    page.read_only = 1;
    page.epoch = 0x12345678;
    page.pattern = 126;
    page.page.request_id = 777;
    page.page.track = 15;
    page.page.first_step = 48;
    page.page.valid = 1;
    page.page.steps[15].note = 99;
    page.page.steps[15].locks[3].value = 321;
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SEQ_SLOT_PAGE, &page, sizeof(page)),
              0u);
    SeqSlotPageMessage parsed_page;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        packet.data(), MSG_SEQ_SLOT_PAGE, &parsed_page, sizeof(parsed_page)));
    EXPECT_EQ(std::memcmp(&page, &parsed_page, sizeof(page)), 0);
    EXPECT_TRUE(IsValidSeqSlotPage(parsed_page));
    parsed_page.read_only = 2;
    EXPECT_FALSE(IsValidSeqSlotPage(parsed_page));
    parsed_page = page;
    parsed_page.pattern = 128;
    EXPECT_FALSE(IsValidSeqSlotPage(parsed_page));
    parsed_page = page;
    parsed_page.page.first_step = 63;
    EXPECT_FALSE(IsValidSeqSlotPage(parsed_page));
}
