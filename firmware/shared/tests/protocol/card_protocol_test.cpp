#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(CardProtocol, CommandsAndStateRoundTrip) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op = CARD_GET; op <= CARD_CANCEL; ++op) {
        CardOpMessage request{0x12345678, op == CARD_CONFIRM_FORMAT ? 0xabcdef12u : 0u, op, {}};
        ASSERT_TRUE(IsValidCardOp(request));
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_CARD_OP, &request, sizeof(request)),
                  0u);
        CardOpMessage copy;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(packet.data(), MSG_CARD_OP, &copy, sizeof(copy)));
        EXPECT_EQ(std::memcmp(&request, &copy, sizeof(copy)), 0);
    }
    CardStateMessage state{0x12345678, 0xabcdef12, 0x98765432, CARD_FAILED, CARD_IO, 1, 0};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_CARD_STATE, &state, sizeof(state)),
              0u);
    CardStateMessage copy;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(packet.data(), MSG_CARD_STATE, &copy, sizeof(copy)));
    EXPECT_EQ(std::memcmp(&state, &copy, sizeof(copy)), 0);
    EXPECT_TRUE(IsValidCardState(copy));
}
TEST(CardProtocol, ConfirmationRequiresTokenAndValidEnvelope) {
    CardOpMessage request;
    EXPECT_FALSE(IsValidCardOp(request));
    request.request_id = 1;
    request.op = CARD_CONFIRM_FORMAT;
    EXPECT_FALSE(IsValidCardOp(request));
    request.token = 42;
    EXPECT_TRUE(IsValidCardOp(request));
    for (auto& byte: request.reserved) {
        byte = 1;
        EXPECT_FALSE(IsValidCardOp(request));
        byte = 0;
    }
    request.op = CARD_PREPARE_FORMAT;
    EXPECT_FALSE(IsValidCardOp(request));
    request.token = 0;
    request.op = 255;
    EXPECT_FALSE(IsValidCardOp(request));
}
