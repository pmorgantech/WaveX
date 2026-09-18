
#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
#include <limits>
using namespace WaveX::Protocol;
TEST(InstrumentLfoProtocol, BothSettingsAndOperationsRoundTrip) {
    InstLfoOpMessage in;
    in.request_id = 33;
    in.revision = 10;
    in.track = 15;
    in.index = 1;
    in.op = INST_LFO_SET;
    in.value = {4, 7, 0, 1, 19.5f, 600, 0.123456f};
    ASSERT_TRUE(IsValidInstLfoOp(in));
    std::array<uint8_t, 512> bytes{};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(bytes.data(), bytes.size(), MSG_INST_LFO_OP, &in, sizeof(in)),
        0);
    InstLfoOpMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(bytes.data(), MSG_INST_LFO_OP, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    InstLfoSyncMessage state;
    state.request_id = 34;
    state.completed_request_id = 33;
    state.revision = 11;
    state.track = 15;
    state.valid = 1;
    state.values[1] = in.value;
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  bytes.data(), bytes.size(), MSG_INST_LFO_SYNC, &state, sizeof(state)),
              0);
    InstLfoSyncMessage received;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        bytes.data(), MSG_INST_LFO_SYNC, &received, sizeof(received)));
    EXPECT_EQ(std::memcmp(&state, &received, sizeof(state)), 0);
}
TEST(InstrumentLfoProtocol, RejectsInvalidIdentityAndSettings) {
    InstLfoOpMessage m;
    m.request_id = 1;
    m.revision = 1;
    m.op = INST_LFO_SET;
    ASSERT_TRUE(IsValidInstLfoOp(m));
    auto bad = m;
    bad.index = 2;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.track = 16;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.revision = 0;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.reserved = 1;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.wave = 5;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.sync_div = WaveX::LfoControl::kDivisionCount;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.pitch_follow = 2;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.retrigger = 2;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.fade_s = 601;
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.rate_hz = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValidInstLfoOp(bad));
    bad = m;
    bad.value.delay_s = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(IsValidInstLfoOp(bad));
}

TEST(InstrumentLfoProtocol, ExtendedRatesAndDottedDivisionRoundTrip) {
    for (float rate: {.01f, .1f, 100.f}) {
        InstLfoOpMessage in;
        in.request_id = in.revision = 1;
        in.op = INST_LFO_SET;
        in.value.rate_hz = rate;
        in.value.sync_div = 8;
        ASSERT_TRUE(IsValidInstLfoOp(in));
        std::array<uint8_t, 512> bytes{};
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      bytes.data(), bytes.size(), MSG_INST_LFO_OP, &in, sizeof(in)),
                  0u);
        InstLfoOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(bytes.data(), MSG_INST_LFO_OP, &out, sizeof(out)));
        EXPECT_FLOAT_EQ(out.value.rate_hz, rate);
        EXPECT_EQ(out.value.sync_div, 8);
    }
}
