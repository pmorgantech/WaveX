#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(SamplePlayheadProtocol, IdentityAndLargeSourcePositionRoundTrip) {
    std::array<uint8_t, 128> packet{};
    SamplePlayheadRequest request{0x12345678, 65535, 123};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SAMPLE_PLAYHEAD, &request, sizeof(request)),
              0u);
    SamplePlayheadRequest parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_SAMPLE_PLAYHEAD, &parsed, sizeof(parsed)));
    EXPECT_EQ(std::memcmp(&request, &parsed, sizeof(parsed)), 0);
    EXPECT_TRUE(IsValidSamplePlayheadRequest(parsed));
    for (uint8_t source = PLAYHEAD_IDLE; source <= PLAYHEAD_STREAM; ++source) {
        SamplePlayheadMessage value;
        value.request_id = request.request_id;
        value.sample_id = request.sample_id;
        value.generation = request.generation;
        value.frame = 0xfedcba98;
        value.source = source;
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SAMPLE_PLAYHEAD, &value, sizeof(value)),
                  0u);
        SamplePlayheadMessage result;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            packet.data(), MSG_SAMPLE_PLAYHEAD, &result, sizeof(result)));
        EXPECT_EQ(std::memcmp(&value, &result, sizeof(result)), 0);
        EXPECT_TRUE(IsValidSamplePlayhead(result));
    }
}
TEST(SamplePlayheadProtocol, RejectsMissingIdentityAndUnknownState) {
    SamplePlayheadRequest r;
    EXPECT_FALSE(IsValidSamplePlayheadRequest(r));
    r.request_id = 1;
    EXPECT_FALSE(IsValidSamplePlayheadRequest(r));
    r.sample_id = 1;
    EXPECT_TRUE(IsValidSamplePlayheadRequest(r));
    SamplePlayheadMessage m;
    m.request_id = 1;
    m.sample_id = 1;
    EXPECT_TRUE(IsValidSamplePlayhead(m));
    m.source = 3;
    EXPECT_FALSE(IsValidSamplePlayhead(m));
    m.source = 1;
    m.reserved[2] = 1;
    EXPECT_FALSE(IsValidSamplePlayhead(m));
}
