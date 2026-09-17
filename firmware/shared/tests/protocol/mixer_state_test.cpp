#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;

TEST(MixerStateProtocol, RequestAndTargetsRoundTrip) {
    std::array<uint8_t, 128> packet{};
    MixStateRequest request{1234, 15}, decoded;
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_MIX_STATE_REQ, &request, sizeof(request)),
              0);
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_MIX_STATE_REQ, &decoded, sizeof(decoded)));
    EXPECT_EQ(std::memcmp(&request, &decoded, sizeof(request)), 0);
    for (uint16_t gain: {0, 6000, 6600}) {
        for (uint16_t pan: {0, 32768, 65535}) {
            MixStateMessage state{1234, 15, 1, gain, pan, 1}, out;
            ASSERT_TRUE(IsValidMixState(state));
            ASSERT_GT(ProtocolHandler::CreatePacket(
                          packet.data(), packet.size(), MSG_MIX_STATE, &state, sizeof(state)),
                      0);
            ASSERT_TRUE(
                ProtocolHandler::ParseMessage(packet.data(), MSG_MIX_STATE, &out, sizeof(out)));
            EXPECT_EQ(std::memcmp(&state, &out, sizeof(state)), 0);
        }
    }
}
TEST(MixerStateProtocol, RejectsInvalidState) {
    MixStateMessage state{1, 0, 1, 6000, 32768, 0};
    ASSERT_TRUE(IsValidMixState(state));
    auto bad = state;
    bad.request_id = 0;
    EXPECT_FALSE(IsValidMixState(bad));
    bad = state;
    bad.track = 16;
    EXPECT_FALSE(IsValidMixState(bad));
    bad = state;
    bad.valid = 0;
    EXPECT_FALSE(IsValidMixState(bad));
    bad = state;
    bad.gain = 6601;
    EXPECT_FALSE(IsValidMixState(bad));
    bad = state;
    bad.mute = 2;
    EXPECT_FALSE(IsValidMixState(bad));
}

TEST(MixerStateProtocol, SoloMaskRoundTripDoesNotReuseStoredMuteOpcode) {
    using namespace WaveX::Protocol;
    EXPECT_NE(MIX_OP_SET_SOLO_MASK, MIX_OP_SET_MUTE_MASK);
    for (uint16_t mask: {uint16_t{0}, uint16_t{1}, uint16_t{0x8000}, uint16_t{0xffff}}) {
        uint8_t bytes[32]{};
        MixOpMessage in{MIX_OP_SET_SOLO_MASK, 0, mask}, out;
        ASSERT_GT(ProtocolHandler::CreatePacket(bytes, sizeof(bytes), MSG_MIX_OP, &in, sizeof(in)),
                  0);
        ASSERT_TRUE(ProtocolHandler::ParseMessage(bytes, MSG_MIX_OP, &out, sizeof(out)));
        EXPECT_EQ(out.op, MIX_OP_SET_SOLO_MASK);
        EXPECT_EQ(out.value, mask);
    }
}

TEST(MixerStateProtocol, MasterReadbackRoundTripAndValidation) {
    uint8_t packet[128]{};
    MixStateRequest request{42, MIX_MASTER_TRACK}, decoded;
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet, sizeof(packet), MSG_MIX_STATE_REQ, &request, sizeof(request)),
              0);
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet, MSG_MIX_STATE_REQ, &decoded, sizeof(decoded)));
    EXPECT_EQ(decoded.track, MIX_MASTER_TRACK);
    for (uint16_t gain: {0, 6000, 6600}) {
        MixStateMessage state{42, MIX_MASTER_TRACK, 1, gain, 32768, 0}, out;
        ASSERT_TRUE(IsValidMixState(state));
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet, sizeof(packet), MSG_MIX_STATE, &state, sizeof(state)),
                  0);
        ASSERT_TRUE(ProtocolHandler::ParseMessage(packet, MSG_MIX_STATE, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&state, &out, sizeof(state)), 0);
        state.mute = 1;
        EXPECT_FALSE(IsValidMixState(state));
        state.mute = 0;
        state.pan = 0;
        EXPECT_FALSE(IsValidMixState(state));
    }
}
