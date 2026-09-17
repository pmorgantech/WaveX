#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(ProjectProtocol, EveryOperationAndRetainedStatusRoundTrip) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op = PROJECT_GET; op <= PROJECT_NEW; ++op) {
        ProjectOpMessage request;
        request.request_id = 0x87654321;
        request.op = op;
        std::strcpy(request.name, "Night project");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_PROJECT_OP, &request, sizeof(request)),
                  0u);
        ProjectOpMessage parsed;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_PROJECT_OP, &parsed, sizeof(parsed)));
        EXPECT_EQ(std::memcmp(&request, &parsed, sizeof(parsed)), 0);
        EXPECT_TRUE(IsValidProjectOp(parsed));
    }
    for (uint8_t error = PROJECT_OK; error <= PROJECT_AUDIO_BUSY; ++error) {
        ProjectStatusMessage status;
        status.request_id = 555;
        status.active_request_id = 444;
        status.completed_request_id = 333;
        status.busy = 1;
        status.error = error;
        status.active_op = PROJECT_LOAD;
        status.completed_op = PROJECT_SAVE_COPY;
        status.progress = 73;
        status.failed_track = 15;
        std::strcpy(status.name, "Earlier project");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_PROJECT_STATUS, &status, sizeof(status)),
                  0u);
        ProjectStatusMessage parsed;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            packet.data(), MSG_PROJECT_STATUS, &parsed, sizeof(parsed)));
        EXPECT_EQ(std::memcmp(&status, &parsed, sizeof(parsed)), 0);
        EXPECT_TRUE(IsValidProjectStatus(parsed));
    }
}
TEST(ProjectProtocol, RejectsInvalidIdsBoundsAndUnterminatedStatus) {
    ProjectOpMessage request;
    EXPECT_FALSE(IsValidProjectOp(request));
    request.request_id = 1;
    EXPECT_TRUE(IsValidProjectOp(request));
    request.reserved[2] = 1;
    EXPECT_FALSE(IsValidProjectOp(request));
    request.reserved[2] = 0;
    request.op = PROJECT_NEW + 1;
    EXPECT_FALSE(IsValidProjectOp(request));
    ProjectStatusMessage status;
    status.request_id = 1;
    EXPECT_TRUE(IsValidProjectStatus(status));
    status.busy = 1;
    EXPECT_FALSE(IsValidProjectStatus(status));
    status.active_request_id = 2;
    status.active_op = PROJECT_LOAD;
    EXPECT_TRUE(IsValidProjectStatus(status));
    status.progress = 101;
    EXPECT_FALSE(IsValidProjectStatus(status));
    status.progress = 99;
    status.failed_track = 16;
    EXPECT_FALSE(IsValidProjectStatus(status));
    status.failed_track = 0xff;
    std::memset(status.name, 'x', sizeof(status.name));
    EXPECT_FALSE(IsValidProjectStatus(status));
}
