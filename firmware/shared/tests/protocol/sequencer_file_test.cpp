#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(SequencerFileProtocol, RequestsAndRetainedCompletionRoundTrip) {
    for (uint8_t op = SEQ_FILE_GET; op <= SEQ_FILE_NEW; ++op) {
        SeqFileOpMessage request;
        request.request_id = 0xa1234567;
        request.op = op;
        std::strcpy(request.name, "Pattern 23");
        std::array<uint8_t, 128> packet{};
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SEQ_FILE_OP, &request, sizeof(request)),
                  0u);
        SeqFileOpMessage copy;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_FILE_OP, &copy, sizeof(copy)));
        EXPECT_EQ(std::memcmp(&request, &copy, sizeof(copy)), 0);
        EXPECT_TRUE(IsValidSeqFileOp(copy));
        SeqFileStatusMessage state;
        state.request_id = 999;
        state.active_request_id = request.request_id;
        state.completed_request_id = 123;
        state.busy = 1;
        state.error = SEQ_FILE_NO_SPACE;
        state.completed_op = SEQ_FILE_SAVE_COPY;
        std::strcpy(state.name, "Earlier pattern");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SEQ_FILE_STATUS, &state, sizeof(state)),
                  0u);
        SeqFileStatusMessage parsed;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            packet.data(), MSG_SEQ_FILE_STATUS, &parsed, sizeof(parsed)));
        EXPECT_EQ(std::memcmp(&state, &parsed, sizeof(state)), 0);
    }
}
TEST(SequencerFileProtocol, RejectsInvalidEnvelope) {
    SeqFileOpMessage m;
    EXPECT_FALSE(IsValidSeqFileOp(m));
    m.request_id = 1;
    EXPECT_TRUE(IsValidSeqFileOp(m));
    m.op = 4;
    EXPECT_FALSE(IsValidSeqFileOp(m));
    m.op = 0;
    m.reserved[2] = 1;
    EXPECT_FALSE(IsValidSeqFileOp(m));
}
