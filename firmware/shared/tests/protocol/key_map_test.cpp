
#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(KeyMapProtocol, OperationsAndSparseSnapshotRoundTrip) {
    for (uint8_t op = KEY_MAP_GET; op <= KEY_MAP_ASSIGN; ++op) {
        InstKeyMapOpMessage in;
        in.request_id = 123456;
        in.revision = 789;
        in.track = 15;
        in.zone = 31;
        in.op = op;
        in.expected_sample = 65000;
        in.value = {65000, 24, 72, 20, 99, 48};
        ASSERT_TRUE(IsValidKeyMapOp(in));
        std::array<uint8_t, 512> wire{};
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      wire.data(), wire.size(), MSG_INST_KEY_MAP_OP, &in, sizeof(in)),
                  0);
        InstKeyMapOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(wire.data(), MSG_INST_KEY_MAP_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
    InstKeyMapSyncMessage in;
    in.request_id = 88;
    in.completed_request_id = 77;
    in.revision = 55;
    in.track = 15;
    in.loaded = 1;
    in.zones[31] = {65000, 24, 72, 20, 99, 48};
    std::strcpy(in.name, "Velocity split");
    std::array<uint8_t, 512> wire{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  wire.data(), wire.size(), MSG_INST_KEY_MAP_SYNC, &in, sizeof(in)),
              0);
    InstKeyMapSyncMessage out;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(wire.data(), MSG_INST_KEY_MAP_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    InstOpMessage keyboard(17, 15, INST_OP_NEW_KEYBOARD, "Keys");
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  wire.data(), wire.size(), MSG_INST_OP, &keyboard, sizeof(keyboard)),
              0);
    InstOpMessage restored;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(wire.data(), MSG_INST_OP, &restored, sizeof(restored)));
    EXPECT_EQ(restored.op, INST_OP_NEW_KEYBOARD);
}
TEST(KeyMapProtocol, RejectsInvalidRangesAndIdentity) {
    InstKeyMapOpMessage m;
    m.request_id = 1;
    m.revision = 1;
    m.op = KEY_MAP_SET_RANGE;
    m.expected_sample = m.value.sample_id = 2;
    ASSERT_TRUE(IsValidKeyMapOp(m));
    auto bad = m;
    bad.value.key_lo = 128;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.value.key_lo = 70;
    bad.value.key_hi = 60;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.value.vel_lo = 0;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.value.vel_hi = 0;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.value.root_note = 128;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.value.sample_id = 3;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.revision = 0;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.zone = 32;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
    bad = m;
    bad.track = 16;
    EXPECT_FALSE(IsValidKeyMapOp(bad));
}
