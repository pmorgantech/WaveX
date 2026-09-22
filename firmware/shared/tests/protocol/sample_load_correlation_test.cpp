#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(SampleLoadCorrelation, RequestAndAllReplyStatesRoundTripWithDistinctResidentIdentity) {
    SampleLoadRequest request;
    request.request_id = 0x12345678;
    request.sample = SampleLoadMessage(7, 96000, 48000, 1, 16, "/kick.wav");
    std::array<uint8_t, 512> frame{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  frame.data(), frame.size(), MSG_SAMPLE_LOAD_REQ, &request, sizeof(request)),
              0u);
    SampleLoadRequest parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(frame.data(), MSG_SAMPLE_LOAD_REQ, &parsed, sizeof(parsed)));
    EXPECT_TRUE(IsValidSampleLoadRequest(parsed));
    EXPECT_EQ(std::memcmp(&request, &parsed, sizeof(parsed)), 0);
    for (uint8_t state:
         {SAMPLE_STATUS_LOAD_PROGRESS, SAMPLE_STATUS_LOAD_COMPLETE, SAMPLE_STATUS_LOAD_FAILED}) {
        SampleLoadReply reply;
        reply.request_id = request.request_id;
        reply.status = SampleStatusMessage(
            91, state, 1, 48000, state == SAMPLE_STATUS_LOAD_FAILED ? SAMPLE_LOAD_FAIL_BUSY : 50);
        ASSERT_TRUE(IsValidSampleLoadReply(reply));
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      frame.data(), frame.size(), MSG_SAMPLE_LOAD_REPLY, &reply, sizeof(reply)),
                  0u);
        SampleLoadReply result;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            frame.data(), MSG_SAMPLE_LOAD_REPLY, &result, sizeof(result)));
        EXPECT_EQ(std::memcmp(&reply, &result, sizeof(result)), 0);
        EXPECT_EQ(result.request_id, request.request_id);
        EXPECT_EQ(result.status.sample_id, 91);
    }
}
TEST(SampleLoadCorrelation, RejectsUncorrelatedMalformedAndUnknownReplies) {
    SampleLoadRequest request;
    std::strcpy(request.sample.path, "/kick.wav");
    EXPECT_FALSE(IsValidSampleLoadRequest(request));
    request.request_id = 1;
    request.version = 2;
    EXPECT_FALSE(IsValidSampleLoadRequest(request));
    request.version = 1;
    std::memset(request.sample.path, 'x', sizeof(request.sample.path));
    EXPECT_FALSE(IsValidSampleLoadRequest(request));
    SampleLoadReply reply;
    reply.status = SampleStatusMessage(42, SAMPLE_STATUS_LOAD_COMPLETE, 1, 48000, 10);
    EXPECT_FALSE(IsValidSampleLoadReply(reply));
    reply.request_id = 1;
    reply.version = 2;
    EXPECT_FALSE(IsValidSampleLoadReply(reply));
    reply.version = 1;
    reply.status.sample_id = 0;
    EXPECT_FALSE(IsValidSampleLoadReply(reply));
    reply.status.state = SAMPLE_STATUS_STOPPED;
    EXPECT_FALSE(IsValidSampleLoadReply(reply));
    reply.status.state = SAMPLE_STATUS_LOAD_PROGRESS;
    reply.status.frames_played = 101;
    EXPECT_FALSE(IsValidSampleLoadReply(reply));
}
