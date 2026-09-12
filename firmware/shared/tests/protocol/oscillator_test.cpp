
#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
#include <limits>
using namespace WaveX::Protocol;
TEST(OscillatorProtocol, SettingsCopyAndReadbackRoundTrip) {
    std::array<uint8_t, 128> wire{};
    for (uint8_t op = INST_OSC_GET; op <= INST_OSC_COPY_EMPTY; ++op) {
        InstOscOpMessage in;
        in.request_id = 123;
        in.revision = 456;
        in.track = 15;
        in.oscillator = 1;
        in.op = op;
        in.value = {1.75f, 0.25f, -48, 100, 0, 0};
        ASSERT_TRUE(IsValidInstOscOp(in));
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      wire.data(), wire.size(), MSG_INST_OSC_OP, &in, sizeof(in)),
                  0);
        InstOscOpMessage out;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_OSC_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
    InstOscSyncMessage in;
    in.request_id = 123;
    in.completed_request_id = 122;
    in.revision = 789;
    in.track = 15;
    in.oscillator = 1;
    in.valid = 1;
    in.type = 1;
    in.zones = 32;
    in.value = {0.25f, 0.75f, 48, -100, 1, 0};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_INST_OSC_SYNC, &in, sizeof(in)),
        0);
    InstOscSyncMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_OSC_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
}
TEST(OscillatorProtocol, RejectsInvalidIdentityRangesAndNonFiniteValues) {
    InstOscOpMessage m;
    m.request_id = 1;
    m.revision = 2;
    m.op = INST_OSC_SET;
    ASSERT_TRUE(IsValidInstOscOp(m));
    auto bad = m;
    bad.request_id = 0;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.revision = 0;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.track = 16;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.oscillator = 2;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.value.mix = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.value.level = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.value.coarse = 49;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.value.keytrack = 2;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.op = INST_OSC_COPY_EMPTY;
    EXPECT_FALSE(IsValidInstOscOp(bad));
}
