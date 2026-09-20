#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(MelodicProtocol, BothDirectionsRoundTripAndRejectInvalidLanes) {
    std::array<uint8_t, 64> packet{};
    SeqPatternRequestMessage request{37, 15, 63, 0}, copy;
    ASSERT_TRUE(IsValidSeqNotesRequest(request));
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SEQ_NOTES, &request, sizeof(request)),
              0);
    ASSERT_TRUE(ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_NOTES, &copy, sizeof(copy)));
    EXPECT_EQ(std::memcmp(&request, &copy, sizeof(copy)), 0);
    SeqNotesMessage reply, decoded;
    reply.request_id = 37;
    reply.epoch = 8;
    reply.track = 15;
    reply.step = 63;
    reply.melodic = 1;
    reply.quantize = 2;
    reply.notes[0] = {0, 127, 0};
    reply.notes[3] = {127, 1, 32767};
    ASSERT_TRUE(IsValidSeqNotes(reply));
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SEQ_NOTES, &reply, sizeof(reply)),
              0);
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_NOTES, &decoded, sizeof(decoded)));
    EXPECT_EQ(std::memcmp(&reply, &decoded, sizeof(reply)), 0);
    reply.quantize = 3;
    EXPECT_FALSE(IsValidSeqNotes(reply));
    reply.quantize = 2;
    reply.notes[1].velocity = 128;
    EXPECT_FALSE(IsValidSeqNotes(reply));
    request.reserved = 1;
    EXPECT_FALSE(IsValidSeqNotesRequest(request));
}
