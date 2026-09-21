#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
using namespace WaveX::Protocol;
TEST(ArpeggiatorProtocol, RevisionAndWholeConfigurationRoundTrip) {
    InstArpOpMessage m;
    m.request_id = 1;
    m.revision = 7;
    m.op = INST_ARP_SET;
    m.track = 15;
    m.value = {1, 5, 4, 11, 99, 1, 2, 120};
    ASSERT_TRUE(IsValidInstArpOp(m));
    std::array<uint8_t, 512> wire{};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_INST_ARP_OP, &m, sizeof(m)), 0);
    InstArpOpMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_ARP_OP, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&m, &out, sizeof(m)), 0);
    m.value.division = 12;
    EXPECT_FALSE(IsValidInstArpOp(m));
    m.value.division = 3;
    m.revision = 0;
    EXPECT_FALSE(IsValidInstArpOp(m));
    m.op = INST_ARP_GET;
    EXPECT_TRUE(IsValidInstArpOp(m));
}
