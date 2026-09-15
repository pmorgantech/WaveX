
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
        in.value = {1.75f, 0.25f, -48, 100, 0, 1};
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
    in.value = {0.25f, 0.75f, 48, -100, 1, 1};
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
    bad.value.level = 64.01f;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.value.keytrack = 2;
    EXPECT_FALSE(IsValidInstOscOp(bad));
    bad = m;
    bad.op = INST_OSC_COPY_EMPTY;
    EXPECT_FALSE(IsValidInstOscOp(bad));
}

TEST(OscillatorProtocol, PreservesCompleteStoredSettingsRange) {
    InstOscOpMessage m;
    m.request_id = 1;
    m.revision = 1;
    m.op = INST_OSC_SET;
    m.value.level = 64;
    m.value.coarse = -128;
    m.value.fine = 127;
    EXPECT_TRUE(IsValidInstOscOp(m));
}

TEST(OscillatorProtocol, MonoDefaultsOffAndRejectsNonBooleanValues) {
    InstOscOpMessage m;
    EXPECT_EQ(m.value.mono, 0);
    m.request_id = m.revision = 1;
    m.op = INST_OSC_SET;
    m.value.mono = 1;
    EXPECT_TRUE(IsValidInstOscOp(m));
    m.value.mono = 2;
    EXPECT_FALSE(IsValidInstOscOp(m));
}
