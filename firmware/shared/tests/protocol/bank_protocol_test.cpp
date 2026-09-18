#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(BankProtocol, OperationsAndRetainedResultsRoundTrip) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op = BANK_GET; op <= BANK_RECALL; ++op) {
        BankOpMessage request;
        request.request_id = 123;
        request.revision = 456;
        request.op = op;
        request.slot = 127;
        request.track = 15;
        request.flags = BANK_CONFIRM_REPLACE;
        std::strcpy(request.name, "Live set");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_BANK_OP, &request, sizeof(request)),
                  0u);
        BankOpMessage read;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(packet.data(), MSG_BANK_OP, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&read, &request, sizeof(read)), 0);
        EXPECT_TRUE(IsValidBankOp(read));
    }
    for (uint8_t error = BANK_OK; error <= BANK_CONFIRM_REQUIRED; ++error) {
        BankStatusMessage status;
        status.request_id = 1;
        status.active_request_id = 2;
        status.completed_request_id = 3;
        status.busy = 1;
        status.active_op = BANK_STORE_COPY;
        status.completed_op = BANK_RECALL;
        status.error = error;
        status.loaded = status.occupied = 1;
        status.slot = 127;
        std::strcpy(status.name, "Night");
        std::strcpy(status.instrument, "Bass");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_BANK_STATUS, &status, sizeof(status)),
                  0u);
        BankStatusMessage read;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_BANK_STATUS, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&read, &status, sizeof(read)), 0);
        EXPECT_TRUE(IsValidBankStatus(read));
    }
}
TEST(BankProtocol, RejectsInvalidBoundsFlagsAndStatus) {
    BankOpMessage op;
    EXPECT_FALSE(IsValidBankOp(op));
    op.request_id = 1;
    EXPECT_TRUE(IsValidBankOp(op));
    op.slot = 128;
    EXPECT_FALSE(IsValidBankOp(op));
    op.slot = 0;
    op.track = 16;
    EXPECT_FALSE(IsValidBankOp(op));
    op.track = 0;
    op.flags = 2;
    EXPECT_FALSE(IsValidBankOp(op));
    op.flags = 0;
    op.op = BANK_RECALL + 1;
    EXPECT_FALSE(IsValidBankOp(op));
    BankStatusMessage status;
    status.request_id = 1;
    EXPECT_TRUE(IsValidBankStatus(status));
    status.occupied = 1;
    EXPECT_FALSE(IsValidBankStatus(status));
    status.loaded = 1;
    status.busy = 1;
    EXPECT_FALSE(IsValidBankStatus(status));
    status.active_request_id = 2;
    status.active_op = BANK_RECALL;
    EXPECT_TRUE(IsValidBankStatus(status));
    std::memset(status.instrument, 'x', sizeof(status.instrument));
    EXPECT_FALSE(IsValidBankStatus(status));
}
