#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(PatternSlotProtocol, OperationsAndRetainedResultsRoundTrip) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op = SEQ_SLOT_GET; op <= SEQ_SLOT_SELECT; ++op) {
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
    for (uint8_t error = SEQ_SLOT_OK; error <= SEQ_SLOT_CAPTURE_BUSY; ++error) {
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
    request.op = SEQ_SLOT_SELECT + 1;
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
