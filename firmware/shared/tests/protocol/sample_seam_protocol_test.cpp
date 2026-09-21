#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(SampleSeamProtocol, RoundTripAndBounds) {
    SampleSeamRequest in;
    in.request_id = 0x12345678;
    in.expected.sample_id = 2345;
    in.generation = 7;
    in.action = SAMPLE_SEAM_SNAP;
    in.expected.loop_crossfade_ms = 20;
    in.expected.channel_mode = SAMPLE_CH_RIGHT;
    ASSERT_TRUE(IsValidSampleSeamRequest(in));
    std::array<uint8_t, 128> packet{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SAMPLE_SEAM_REQ, &in, sizeof(in)),
              0u);
    SampleSeamRequest out;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_SAMPLE_SEAM_REQ, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    in.radius = 2049;
    EXPECT_FALSE(IsValidSampleSeamRequest(in));
    in.radius = 2048;
    in.expected.loop_crossfade_ms = 21;
    EXPECT_FALSE(IsValidSampleSeamRequest(in));
    in.expected.loop_crossfade_ms = 20;
    in.expected.channel_mode = 4;
    EXPECT_FALSE(IsValidSampleSeamRequest(in));
    SampleSeamStatus status;
    status.request_id = 9;
    status.sample_id = 3456;
    status.raw_left = -65535;
    status.raw_right = 65535;
    status.left = -123;
    status.right = 456;
    ASSERT_TRUE(IsValidSampleSeamStatus(status));
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SAMPLE_SEAM_STATUS, &status, sizeof(status)),
              0u);
    SampleSeamStatus decoded;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        packet.data(), MSG_SAMPLE_SEAM_STATUS, &decoded, sizeof(decoded)));
    EXPECT_EQ(std::memcmp(&status, &decoded, sizeof(status)), 0);
    status.left = -65536;
    EXPECT_FALSE(IsValidSampleSeamStatus(status));
}
