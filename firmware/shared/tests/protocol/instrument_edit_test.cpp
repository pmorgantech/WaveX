#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
#include <limits>
using namespace WaveX::Protocol;
TEST(InstrumentEditProtocol, OperationsAndDirtyReadbackRoundTrip) {
    std::array<uint8_t, 256> packet{};
    for (uint8_t op = 0; op <= INST_EDIT_AMP; ++op) {
        InstEditOpMessage in;
        in.request_id = 93;
        in.revision = 12;
        in.track = 15;
        in.op = op;
        in.sound = {1234.5f, .42f, 1.25f, .35f};
        ASSERT_TRUE(IsValidInstEditOp(in));
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_INST_EDIT_OP, &in, sizeof(in)),
                  0);
        InstEditOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_INST_EDIT_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
    InstEditSyncMessage state;
    state.request_id = 44;
    state.completed_request_id = 43;
    state.revision = 19;
    state.track = 15;
    state.valid = 1;
    state.dirty = 1;
    state.sound = {1200, .3f, 0, 1};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_INST_EDIT_SYNC, &state, sizeof(state)),
              0);
    InstEditSyncMessage out;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_INST_EDIT_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&state, &out, sizeof(out)), 0);
}
TEST(InstrumentEditProtocol, RejectsInvalidIdentityAndNonfiniteValues) {
    InstEditOpMessage m;
    m.request_id = 1;
    m.revision = 1;
    m.op = INST_EDIT_FILTER;
    ASSERT_TRUE(IsValidInstEditOp(m));
    auto bad = m;
    bad.track = 16;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.op = 5;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.revision = 0;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.request_id = 0;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.reserved = 1;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.op = INST_EDIT_AMP;
    bad.sound.gain = 65;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.op = INST_EDIT_AMP;
    bad.sound.pan = -1;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.sound.cutoff_hz = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.sound.resonance = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(IsValidInstEditOp(bad));
    m.op = INST_EDIT_GET;
    m.revision = 0;
    EXPECT_TRUE(IsValidInstEditOp(m));
}
