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

TEST(InstrumentEditorProtocol, PadSoundOperationsAndEffectiveReplyRoundTrip) {
    for (uint8_t op = PAD_SOUND_GET; op <= PAD_SOUND_SUSTAIN; ++op) {
        InstPadSoundOpMessage in{0x12345678,
                                 15,
                                 15,
                                 op,
                                 65530,
                                 static_cast<uint16_t>(op <= PAD_SOUND_INHERIT ? 0 : 999)};
        ASSERT_TRUE(IsValidPadSoundOp(in));
        std::array<uint8_t, 128> wire{};
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      wire.data(), wire.size(), MSG_INST_PAD_SOUND_OP, &in, sizeof(in)),
                  0);
        InstPadSoundOpMessage out;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(wire.data(), MSG_INST_PAD_SOUND_OP, &out, sizeof(out)));
        EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
    }
    InstPadSoundSyncMessage in{
        100, 99, 15, 15, 1, 0, INST_ERROR_BUSY, 1, 65530, 4321, 25, 750, 350};
    std::array<uint8_t, 128> wire{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  wire.data(), wire.size(), MSG_INST_PAD_SOUND_SYNC, &in, sizeof(in)),
              0);
    InstPadSoundSyncMessage out;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(wire.data(), MSG_INST_PAD_SOUND_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
}
TEST(InstrumentEditorProtocol, PadSoundRejectsMalformedIdentityAndRanges) {
    const InstPadSoundOpMessage valid{1, 0, 0, PAD_SOUND_CUTOFF, 1, 20000};
    auto bad = valid;
    bad.request_id = 0;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.track = 16;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.pad = 16;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.reserved = 1;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.sample_id = 0;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.value = 19;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.value = 20001;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad = valid;
    bad.op = PAD_SOUND_SUSTAIN;
    bad.value = 1001;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad.op = PAD_SOUND_ATTACK;
    bad.value = 10001;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad.op = PAD_SOUND_INHERIT;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
    bad.op = 255;
    EXPECT_FALSE(IsValidPadSoundOp(bad));
}

TEST(InstrumentEditorProtocol, TrackSettingsAndRequestIdentityRoundTrip) {
    TrackStateRequest request{0x12345678, 15}, decoded_request{};
    std::array<uint8_t, 128> wire{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  wire.data(), wire.size(), MSG_TRACK_STATE_REQ, &request, sizeof(request)),
              0);
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        wire.data(), MSG_TRACK_STATE_REQ, &decoded_request, sizeof(decoded_request)));
    EXPECT_EQ(std::memcmp(&request, &decoded_request, sizeof(request)), 0);
    TrackStateMessage state{}, decoded{};
    state.request_id = request.request_id;
    state.track = 15;
    state.valid = state.loaded = 1;
    state.mode = 1;
    state.midi_in = TRACK_MIDI_IN_OMNI;
    state.poly_limit = 8;
    state.priority = 99;
    state.program_change = 1;
    state.sample_id = 65530;
    std::strcpy(state.name, "Keyboard");
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  wire.data(), wire.size(), MSG_TRACK_STATE, &state, sizeof(state)),
              0);
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(wire.data(), MSG_TRACK_STATE, &decoded, sizeof(decoded)));
    EXPECT_EQ(std::memcmp(&state, &decoded, sizeof(state)), 0);
}
TEST(InstrumentEditorProtocol, TrackEditsRejectWideValuesBeforeNarrowing) {
    for (uint16_t value: {0, 1, 16, 255})
        EXPECT_TRUE(IsValidTrackOp({TRACK_OP_SET_MIDI_IN, 15, value}));
    for (uint16_t value: {17, 254, 256, 257, 65535})
        EXPECT_FALSE(IsValidTrackOp({TRACK_OP_SET_MIDI_IN, 15, value}));
    EXPECT_FALSE(IsValidTrackOp({TRACK_OP_SET_MIDI_IN, 16, 1}));
    EXPECT_FALSE(IsValidTrackOp({TRACK_OP_SET_PROGRAM_CHANGE, 0, 2}));
    EXPECT_FALSE(IsValidTrackOp({TRACK_OP_SET_PRIORITY, 0, 256}));
    EXPECT_FALSE(IsValidTrackOp({0, 0, 0}));
}
