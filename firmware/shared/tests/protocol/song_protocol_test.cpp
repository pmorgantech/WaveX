#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(SongProtocol, OperationsAndArrangementRoundTrip) {
    std::array<uint8_t, 512> packet{};
    for (uint8_t op = SEQ_SONG_GET; op <= SEQ_SONG_STOP; ++op) {
        SeqSongOpMessage request;
        request.request_id = 0x87654321;
        request.op = op;
        request.song = 15;
        request.entry = 127;
        request.destination = 126;
        request.pattern = 127;
        request.repeats = 255;
        request.loop = 1;
        request.tempo_bpm_x100 = 30000;
        std::strcpy(request.name, "Final section");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_SEQ_SONG_OP, &request, sizeof(request)),
                  0u);
        SeqSongOpMessage parsed;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_SONG_OP, &parsed, sizeof(parsed)));
        EXPECT_EQ(std::memcmp(&request, &parsed, sizeof(parsed)), 0);
        EXPECT_TRUE(IsValidSeqSongOp(parsed));
    }
    SeqSongStatusMessage status;
    status.request_id = 23;
    status.song = 15;
    status.used = 1;
    status.length = 128;
    status.playing_song = 15;
    status.playing_entry = 127;
    status.playing_repeat = 255;
    status.entries[127] = {127, 255};
    std::strcpy(status.name, "Arrangement");
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_SEQ_SONG_STATUS, &status, sizeof(status)),
              0u);
    SeqSongStatusMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(packet.data(), MSG_SEQ_SONG_STATUS, &parsed, sizeof(parsed)));
    EXPECT_EQ(std::memcmp(&status, &parsed, sizeof(parsed)), 0);
    EXPECT_TRUE(IsValidSeqSongStatus(parsed));
}
TEST(SongProtocol, RejectsMalformedRecords) {
    SeqSongOpMessage op;
    EXPECT_FALSE(IsValidSeqSongOp(op));
    op.request_id = 1;
    EXPECT_TRUE(IsValidSeqSongOp(op));
    op.song = 16;
    EXPECT_FALSE(IsValidSeqSongOp(op));
    op.song = 15;
    op.repeats = 0;
    EXPECT_FALSE(IsValidSeqSongOp(op));
    op.repeats = 1;
    op.entry = 128;
    EXPECT_FALSE(IsValidSeqSongOp(op));
    SeqSongStatusMessage s;
    s.request_id = 1;
    EXPECT_TRUE(IsValidSeqSongStatus(s));
    s.used = 1;
    EXPECT_FALSE(IsValidSeqSongStatus(s));
    s.length = 128;
    s.entries[127].repeats = 0;
    EXPECT_FALSE(IsValidSeqSongStatus(s));
    s.entries[127].repeats = 1;
    EXPECT_TRUE(IsValidSeqSongStatus(s));
    std::memset(s.name, 'x', sizeof(s.name));
    EXPECT_FALSE(IsValidSeqSongStatus(s));
}
