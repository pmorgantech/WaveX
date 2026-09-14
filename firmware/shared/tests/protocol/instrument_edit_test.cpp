#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
#include <limits>
using namespace WaveX::Protocol;
TEST(InstrumentEditProtocol, OperationsAndDirtyReadbackRoundTrip) {
    std::array<uint8_t, 256> packet{};
    for (uint8_t op = 0; op <= INST_EDIT_FILTER_SETTINGS; ++op) {
        InstEditOpMessage in;
        in.request_id = 93;
        in.revision = 12;
        in.track = 15;
        in.op = op;
        in.filter_type = op == INST_EDIT_FILTER_SETTINGS ? INST_FILTER_NOTCH : 0;
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
    state.filter_type = INST_FILTER_BP;
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
    bad.op = INST_EDIT_FILTER_SETTINGS + 1;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.revision = 0;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.request_id = 0;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad = m;
    bad.filter_topology = INST_FILTER_TOPOLOGY_LADDER;  // only op 5 may carry one
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

TEST(InstrumentEditProtocol, FilterModesUseReservedBytesWithoutChangingMessageSizes) {
    for (uint8_t mode = INST_FILTER_LP; mode <= INST_FILTER_NOTCH; ++mode) {
        InstEditOpMessage in;
        in.request_id = 47;
        in.revision = 23;
        in.track = 5;
        in.op = INST_EDIT_FILTER_SETTINGS;
        in.filter_type = mode;
        in.sound.cutoff_hz = 1250;
        in.sound.resonance = .65f;
        ASSERT_TRUE(IsValidInstEditOp(in));
        uint8_t bytes[sizeof(in)];
        std::memcpy(bytes, &in, sizeof(in));
        EXPECT_EQ(bytes[10], mode);
        EXPECT_EQ(bytes[11], 0);
        InstEditOpMessage out;
        std::memcpy(&out, bytes, sizeof(out));
        EXPECT_EQ(out.filter_type, mode);
        EXPECT_FLOAT_EQ(out.sound.cutoff_hz, 1250);
        InstEditSyncMessage state;
        state.filter_type = mode;
        uint8_t snapshot[sizeof(state)];
        std::memcpy(snapshot, &state, sizeof(state));
        EXPECT_EQ(snapshot[17], mode);
        EXPECT_EQ(snapshot[18], 0);
        EXPECT_EQ(snapshot[19], 0);
        in.filter_type = 4;
        EXPECT_FALSE(IsValidInstEditOp(in));
        in.filter_type = 1;
        in.op = INST_EDIT_FILTER;
        EXPECT_FALSE(IsValidInstEditOp(in));
    }
}

TEST(InstrumentEditProtocol, FilterTopologyUsesTheRemainingReservedBytes) {
    std::array<uint8_t, 256> packet{};
    for (uint8_t topology = 0; topology < INST_FILTER_TOPOLOGY_COUNT; ++topology) {
        InstEditOpMessage in;
        in.request_id = 48;
        in.revision = 24;
        in.track = 6;
        in.op = INST_EDIT_FILTER_SETTINGS;
        in.filter_type = INST_FILTER_BP;
        in.filter_topology = topology;
        in.sound.cutoff_hz = 900;
        in.sound.resonance = .2f;
        ASSERT_TRUE(IsValidInstEditOp(in));
        uint8_t bytes[sizeof(in)];
        std::memcpy(bytes, &in, sizeof(in));
        EXPECT_EQ(bytes[10], INST_FILTER_BP);
        EXPECT_EQ(bytes[11], topology);
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_INST_EDIT_OP, &in, sizeof(in)),
                  0);
        InstEditOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_INST_EDIT_OP, &out, sizeof(out)));
        EXPECT_EQ(out.filter_topology, topology);
        EXPECT_EQ(out.filter_type, INST_FILTER_BP);

        InstEditSyncMessage state;
        state.filter_type = INST_FILTER_HP;
        state.filter_topology = topology;
        uint8_t snapshot[sizeof(state)];
        std::memcpy(snapshot, &state, sizeof(state));
        EXPECT_EQ(snapshot[17], INST_FILTER_HP);
        EXPECT_EQ(snapshot[18], topology);
        EXPECT_EQ(snapshot[19], 0);
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_INST_EDIT_SYNC, &state, sizeof(state)),
                  0);
        InstEditSyncMessage back;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_INST_EDIT_SYNC, &back, sizeof(back)));
        EXPECT_EQ(std::memcmp(&state, &back, sizeof(back)), 0);
    }
    InstEditOpMessage bad;
    bad.request_id = 1;
    bad.revision = 1;
    bad.op = INST_EDIT_FILTER_SETTINGS;
    bad.filter_topology = INST_FILTER_TOPOLOGY_COUNT;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.filter_topology = INST_FILTER_TOPOLOGY_LADDER;
    EXPECT_TRUE(IsValidInstEditOp(bad));
    bad.op = INST_EDIT_AMP;
    EXPECT_FALSE(IsValidInstEditOp(bad));
}

TEST(InstrumentEditProtocol, SlopeAndDriveTravelWithTheSettingsOpOnly) {
    std::array<uint8_t, 256> packet{};
    for (uint8_t slope = INST_FILTER_SLOPE_12; slope <= INST_FILTER_SLOPE_24; ++slope) {
        InstEditOpMessage in;
        in.request_id = 49;
        in.revision = 25;
        in.track = 7;
        in.op = INST_EDIT_FILTER_SETTINGS;
        in.filter_type = INST_FILTER_LP;
        in.filter_topology = INST_FILTER_TOPOLOGY_LADDER;
        in.filter_slope = slope;
        in.filter_drive = .35f;
        in.sound = {800, .1f, 1, .5f};
        ASSERT_TRUE(IsValidInstEditOp(in));
        uint8_t bytes[sizeof(in)];
        std::memcpy(bytes, &in, sizeof(in));
        EXPECT_EQ(bytes[28], slope);  // right after the 16-byte sound block
        EXPECT_EQ(bytes[29], 0);
        EXPECT_EQ(bytes[30], 0);
        EXPECT_EQ(bytes[31], 0);
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_INST_EDIT_OP, &in, sizeof(in)),
                  0);
        InstEditOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_INST_EDIT_OP, &out, sizeof(out)));
        EXPECT_EQ(out.filter_slope, slope);
        EXPECT_FLOAT_EQ(out.filter_drive, .35f);

        InstEditSyncMessage state;
        state.filter_slope = slope;
        state.filter_drive = .6f;
        uint8_t snapshot[sizeof(state)];
        std::memcpy(snapshot, &state, sizeof(state));
        EXPECT_EQ(snapshot[36], slope);
        EXPECT_EQ(snapshot[37], 0);
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_INST_EDIT_SYNC, &state, sizeof(state)),
                  0);
        InstEditSyncMessage back;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_INST_EDIT_SYNC, &back, sizeof(back)));
        EXPECT_EQ(std::memcmp(&state, &back, sizeof(back)), 0);
    }
    static_assert(sizeof(InstEditOpMessage) == 36, "slope/drive grew the edit request");
    static_assert(sizeof(InstEditSyncMessage) == 44, "slope/drive grew the sync");
    InstEditOpMessage bad;
    bad.request_id = 1;
    bad.revision = 1;
    bad.op = INST_EDIT_FILTER_SETTINGS;
    bad.filter_slope = INST_FILTER_SLOPE_24 + 1;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.filter_slope = INST_FILTER_SLOPE_24;
    bad.filter_drive = 1.5f;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.filter_drive = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.filter_drive = 1.0f;
    EXPECT_TRUE(IsValidInstEditOp(bad));
    bad.reserved[1] = 1;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.reserved[1] = 0;
    bad.op = INST_EDIT_FILTER;  // legacy op: both must be zero
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.filter_slope = 0;
    EXPECT_FALSE(IsValidInstEditOp(bad));
    bad.filter_drive = 0;
    EXPECT_TRUE(IsValidInstEditOp(bad));
}
