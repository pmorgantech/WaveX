#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(RecordingProtocol, BothSourceFamiliesRoundTrip) {
    for (uint8_t source = REC_CODEC_STEREO; source <= REC_INTERNAL_MIX; ++source) {
        RecordOpMessage m;
        m.request_id = 10;
        m.op = REC_ARM;
        m.source = source;
        ASSERT_TRUE(IsValidRecordOp(m));
        std::array<uint8_t, 512> wire{};
        ASSERT_GT(
            ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_REC_OP, &m, sizeof(m)), 0);
        RecordOpMessage decoded;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(wire.data(), MSG_REC_OP, &decoded, sizeof(decoded)));
        EXPECT_EQ(std::memcmp(&m, &decoded, sizeof(m)), 0);
        EXPECT_EQ(RecordChannels(source),
                  source == REC_CODEC_LEFT || source == REC_CODEC_RIGHT ? 1 : 2);
    }
}
TEST(RecordingProtocol, CompletionAndCaptureErrorAreIndependent) {
    RecordStatusMessage m;
    m.request_id = 12;
    m.completed_request_id = 11;
    m.take_id = 10;
    m.frames = 100;
    m.max_frames = 48000;
    m.state = REC_READY;
    m.capture_error = REC_OVERFLOW;
    m.completed_op = REC_STOP;
    ASSERT_TRUE(IsValidRecordStatus(m));
    std::array<uint8_t, 512> wire{};
    ASSERT_GT(
        ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_REC_STATUS, &m, sizeof(m)), 0);
    RecordStatusMessage decoded;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(wire.data(), MSG_REC_STATUS, &decoded, sizeof(decoded)));
    EXPECT_EQ(std::memcmp(&m, &decoded, sizeof(m)), 0);
}
TEST(RecordingProtocol, RejectsInvalidRequests) {
    RecordOpMessage m;
    EXPECT_FALSE(IsValidRecordOp(m));
    m.request_id = 1;
    m.op = REC_START;
    EXPECT_FALSE(IsValidRecordOp(m));
    m.take_id = 2;
    ASSERT_TRUE(IsValidRecordOp(m));
    auto bad = m;
    bad.source = 4;
    EXPECT_FALSE(IsValidRecordOp(bad));
    bad = m;
    bad.preroll_ms = 501;
    EXPECT_FALSE(IsValidRecordOp(bad));
    bad = m;
    bad.threshold = 32769;
    EXPECT_FALSE(IsValidRecordOp(bad));
    bad = m;
    bad.max_frames = 48000u * 121u;
    EXPECT_FALSE(IsValidRecordOp(bad));
    bad = m;
    std::memset(bad.name, 'a', sizeof(bad.name));
    EXPECT_FALSE(IsValidRecordOp(bad));
}
