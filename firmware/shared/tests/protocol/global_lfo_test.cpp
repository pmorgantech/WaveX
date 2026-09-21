#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <limits>
using namespace WaveX::Protocol;
TEST(GlobalLfoProtocol, RoundTripAndRangeValidation) {
    GlobalLfoOpMessage m;
    m.request_id = 1;
    m.revision = 2;
    m.op = GLOBAL_LFO_SET;
    m.value = {3.5f, 4, 8, 2, 0};
    ASSERT_TRUE(IsValidGlobalLfoOp(m));
    std::array<uint8_t, 128> wire{};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_GLOBAL_LFO_OP, &m, sizeof(m)),
        0);
    GlobalLfoOpMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_GLOBAL_LFO_OP, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&m, &out, sizeof(m)), 0);
    m.value.rate_hz = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValidGlobalLfoOp(m));
    m.value.rate_hz = 1;
    m.value.restart = 3;
    EXPECT_FALSE(IsValidGlobalLfoOp(m));
}
