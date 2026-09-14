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
