#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
#include <limits>
using namespace WaveX::Protocol;
TEST(InstrumentModulatorProtocol, TypedEditsAndSnapshotRoundTrip) {
    for (uint8_t op = INST_MOD_GET; op <= INST_MOD_SET_SLOT; ++op) {
        InstModOpMessage in;
        in.request_id = 123;
        in.revision = 45;
        in.track = 15;
        in.index = 2;
        in.op = op;
        in.envelope = {0, 600, .123456f, 2.5f};
        in.slot = {16, 3, -32767, 2, 1};
        ASSERT_TRUE(IsValidInstModOp(in));
        std::array<uint8_t, 512> wire{};
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      wire.data(), wire.size(), MSG_INST_MOD_OP, &in, sizeof(in)),
                  0);
        InstModOpMessage out;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_MOD_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
    InstModSyncMessage in;
    in.request_id = 9;
    in.completed_request_id = 8;
    in.revision = 7;
    in.track = 15;
    in.valid = 1;
    in.envelopes[2] = {1.5f, 3, .33f, 600};
    in.slots[7] = {16, 3, 32000, 1, 0};
    std::array<uint8_t, 512> wire{};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_INST_MOD_SYNC, &in, sizeof(in)),
        0);
    InstModSyncMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_MOD_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
}
TEST(InstrumentModulatorProtocol, RejectsInvalidIdentityRangesAndNonFiniteValues) {
    InstModOpMessage m;
    m.request_id = 1;
    m.revision = 2;
    m.op = INST_MOD_SET_ENV;
    ASSERT_TRUE(IsValidInstModOp(m));
    auto bad = m;
    bad.index = 3;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.revision = 0;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.track = 16;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.envelope.attack_s = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.envelope.release_s = 601;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.envelope.sustain = 1.01f;
    EXPECT_FALSE(IsValidInstModOp(bad));
    m.op = INST_MOD_SET_SLOT;
    ASSERT_TRUE(IsValidInstModOp(m));
    bad = m;
    bad.index = 8;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.slot.source = INST_MOD_SOURCE_COUNT;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.slot.destination = INST_MOD_DEST_COUNT;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.slot.depth = -32768;
    EXPECT_FALSE(IsValidInstModOp(bad));
    bad = m;
    bad.slot.flags = 2;
    EXPECT_FALSE(IsValidInstModOp(bad));
}

TEST(InstrumentModulatorProtocol, ResonanceRoundTripAndFutureDestinationsRemainRejected) {
    for (uint8_t destination = 0; destination <= 13; ++destination) {
        InstModOpMessage in;
        in.request_id = 1;
        in.revision = 2;
        in.op = INST_MOD_SET_SLOT;
        in.slot = {17, destination, -12345, 1, 0};
        const bool supported = destination < INST_MOD_DEST_COUNT;
        EXPECT_EQ(IsValidInstModOp(in), supported);
        if (!supported)
            continue;
        std::array<uint8_t, 512> wire{};
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      wire.data(), wire.size(), MSG_INST_MOD_OP, &in, sizeof(in)),
                  0);
        InstModOpMessage out;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_MOD_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
        InstModSyncMessage sync;
        sync.slots[7] = in.slot;
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      wire.data(), wire.size(), MSG_INST_MOD_SYNC, &sync, sizeof(sync)),
                  0);
        InstModSyncMessage read;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(wire.data(), MSG_INST_MOD_SYNC, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&sync, &read, sizeof(sync)), 0);
    }
}
