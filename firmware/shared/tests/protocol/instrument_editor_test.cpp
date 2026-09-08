#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;

TEST(InstrumentEditorProtocol, NewPadSaveAndReadRequestsRoundTrip) {
    for (uint8_t op = INST_OP_NEW; op <= INST_OP_GET_PAD_MAP; ++op) {
        InstOpMessage in(0x12345678, 15, op, "Bench kit");
        in.pad_index = 15;
        in.pad_choke = 7;
        in.pad_sample_id = 0x7654;
        std::array<uint8_t, 512> wire{};
        ASSERT_GT(
            ProtocolHandler::CreatePacket(wire.data(), wire.size(), MSG_INST_OP, &in, sizeof(in)),
            0);
        InstOpMessage out;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
}
TEST(InstrumentEditorProtocol, PadIdentityAndMutationOutcomeRoundTrip) {
    InstZoneSyncMessage in;
    in.request_id = 0x1234;
    in.completed_request_id = 0xABCD;
    in.track = 15;
    in.loaded = 1;
    in.mode = 1;
    in.editable = 1;
    in.error = INST_ERROR_EXISTS;
    std::strcpy(in.name, "Sparse kit");
    in.pads[15] = {65530, 9, 75};
    std::array<uint8_t, 128> wire{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  wire.data(), wire.size(), MSG_INST_ZONE_SYNC, &in, sizeof(in)),
              0);
    InstZoneSyncMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(wire.data(), MSG_INST_ZONE_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
}
TEST(InstrumentEditorProtocol, NamesCannotEscapeInstrumentDirectoryOrTruncate) {
    EXPECT_TRUE(IsValidInstrumentName("Kit 12-a_b"));
    EXPECT_FALSE(IsValidInstrumentName(""));
    EXPECT_FALSE(IsValidInstrumentName("../kit"));
    EXPECT_FALSE(IsValidInstrumentName("0:/kit"));
    EXPECT_FALSE(IsValidInstrumentName("bad.name"));
    EXPECT_FALSE(IsValidInstrumentName(" trailing "));
    EXPECT_FALSE(IsValidInstrumentName("123456789012345678901234"));
    char unterminated[INST_NAME_BYTES];
    std::memset(unterminated, 'x', sizeof(unterminated));
    EXPECT_FALSE(IsValidInstrumentName(unterminated));
}
