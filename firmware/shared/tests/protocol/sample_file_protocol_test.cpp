#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>

using namespace WaveX::Protocol;
TEST(SampleFileProtocol, OperationsAndRetainedCompletionRoundTrip) {
    std::array<uint8_t, 512> packet{};
    for (uint8_t op = SAMPLE_FILE_GET; op <= SAMPLE_FILE_COPY; ++op) {
        SampleFileOpMessage in;
        in.request_id = 0x12345678;
        in.sample_id = 4567;
        in.op = op;
        std::strcpy(in.name, "Kick copy");
        ASSERT_TRUE(IsValidSampleFileOp(in));
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SAMPLE_FILE_OP, &in, sizeof(in)),
                  0u);
        SampleFileOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_SAMPLE_FILE_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
    SampleFileStatusMessage in;
    in.request_id = 91;
    in.active_request_id = 82;
    in.completed_request_id = 73;
    in.sample_id = 6543;
    in.busy = 1;
    in.completed_op = SAMPLE_FILE_COPY;
    in.error = SAMPLE_FILE_NO_SPACE;
    in.progress = 64;
    std::strcpy(in.path, "0:/samples/Kick copy.wav");
    ASSERT_TRUE(IsValidSampleFileStatus(in));
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SAMPLE_FILE_STATUS, &in, sizeof(in)),
              0u);
    SampleFileStatusMessage out;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_SAMPLE_FILE_STATUS, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
}
TEST(SampleFileProtocol, RejectsMalformedRequestsAndStatus) {
    SampleFileOpMessage r;
    EXPECT_FALSE(IsValidSampleFileOp(r));
    r.request_id = 1;
    EXPECT_TRUE(IsValidSampleFileOp(r));
    r.op = SAMPLE_FILE_SAVE;
    EXPECT_FALSE(IsValidSampleFileOp(r));
    r.sample_id = 1234;
    EXPECT_TRUE(IsValidSampleFileOp(r));
    r.reserved = 1;
    EXPECT_FALSE(IsValidSampleFileOp(r));
    r.reserved = 0;
    std::memset(r.name, 'a', sizeof(r.name));
    EXPECT_FALSE(IsValidSampleFileOp(r));
    SampleFileStatusMessage s;
    s.request_id = 1;
    s.busy = 1;
    EXPECT_FALSE(IsValidSampleFileStatus(s));
    s.active_request_id = 2;
    EXPECT_TRUE(IsValidSampleFileStatus(s));
    s.progress = 101;
    EXPECT_FALSE(IsValidSampleFileStatus(s));
}
