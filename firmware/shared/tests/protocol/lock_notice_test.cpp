#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
using namespace WaveX::Protocol;
TEST(LockNoticeProtocol, RoundTripAndBounds) {
    SeqLockNoticeMessage m{12, 3, 1, 15, 63, PARAM_PAN, PARAM_GAIN};
    ASSERT_TRUE(IsValidSeqLockNotice(m));
    std::array<uint8_t, 128> wire{};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_SEQ_LOCK_NOTICE, &m, sizeof(m)),
        0);
    SeqLockNoticeMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_SEQ_LOCK_NOTICE, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&m, &out, sizeof(m)), 0);
    m.step = 64;
    EXPECT_FALSE(IsValidSeqLockNotice(m));
}
