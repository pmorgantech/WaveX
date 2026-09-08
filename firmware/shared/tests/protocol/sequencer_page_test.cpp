#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>

using namespace WaveX::Protocol;

TEST(SequencerPageProtocol, RequestRoundTripAndBounds) {
    SeqPatternRequestMessage original;
    original.request_id = 0xA1234567;
    original.track = 15;
    original.first_step = 48;
    std::array<uint8_t, 512> buffer{};
    const auto size = ProtocolHandler::CreatePacket(
        buffer.data(), buffer.size(), MSG_SEQ_PATTERN_SYNC, &original, sizeof(original));
    ASSERT_GT(size, 0u);
    SeqPatternRequestMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer.data(), MSG_SEQ_PATTERN_SYNC, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.request_id, original.request_id);
    EXPECT_EQ(parsed.track, 15);
    EXPECT_EQ(parsed.first_step, 48);
    EXPECT_EQ(parsed.reserved, 0);
    EXPECT_TRUE(IsValidSeqPatternRequest(parsed));
    parsed.first_step = 49;
    EXPECT_FALSE(IsValidSeqPatternRequest(parsed));
    parsed.first_step = 64;
    EXPECT_FALSE(IsValidSeqPatternRequest(parsed));
    parsed.first_step = 0;
    parsed.track = 16;
    EXPECT_FALSE(IsValidSeqPatternRequest(parsed));
    parsed.track = 0;
    parsed.request_id = 0;
    EXPECT_FALSE(IsValidSeqPatternRequest(parsed));
}

TEST(SequencerPageProtocol, CompleteSnapshotRoundTrip) {
    SeqPatternSyncMessage original;
    original.request_id = 0x1234ABCD;
    original.track = 15;
    original.first_step = 48;
    original.length = 63;
    original.scale = 4;
    original.swing = 71;
    original.enabled = 1;
    original.clock_source = SEQ_CLOCK_MIDI;
    original.input_mode = SEQ_INPUT_LIVE_RECORD;
    original.quantize = 1;
    original.valid = 1;
    original.tempo_bpm_x100 = 13925;
    for (uint8_t i = 0; i < SEQ_PAGE_STEPS; ++i) {
        auto& step = original.steps[i];
        step.on = i % 2;
        step.velocity = 110 - i;
        step.probability = 70 + i;
        step.retrig_count = i % 9;
        step.retrig_rate_ticks = i + 1;
        step.micro_offset = -30 + i;
        for (uint8_t k = 0; k < SEQ_STEP_LOCKS; ++k) {
            step.locks[k].parameter = k + 1;
            step.locks[k].value = static_cast<uint16_t>(1000 + i * 100 + k);
        }
    }
    std::array<uint8_t, 512> buffer{};
    const auto size = ProtocolHandler::CreatePacket(
        buffer.data(), buffer.size(), MSG_SEQ_PATTERN_SYNC, &original, sizeof(original));
    ASSERT_GT(size, 0u);
    SeqPatternSyncMessage parsed;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(
        buffer.data(), MSG_SEQ_PATTERN_SYNC, &parsed, sizeof(parsed)));
    EXPECT_EQ(std::memcmp(&parsed, &original, sizeof(parsed)), 0);
    EXPECT_EQ(parsed.steps[15].micro_offset, -15);
    EXPECT_EQ(parsed.steps[15].locks[3].value, 2503);
}

TEST(SequencerPageProtocol, ConfigureCommandRoundTrip) {
    SeqTransportMessage original(
        SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 15050, 0);
    std::array<uint8_t, 128> buffer{};
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  buffer.data(), buffer.size(), MSG_SEQ_TRANSPORT, &original, sizeof(original)),
              0u);
    SeqTransportMessage parsed;
    ASSERT_TRUE(
        ProtocolHandler::ParseMessage(buffer.data(), MSG_SEQ_TRANSPORT, &parsed, sizeof(parsed)));
    EXPECT_EQ(parsed.command, SEQ_TRANSPORT_CONFIGURE);
    EXPECT_EQ(parsed.tempo_bpm_x100, 15050);
}

TEST(SequencerPageProtocol, StepNoteOperationAndReadbackRoundTripAtMidiBounds) {
    using namespace WaveX::Protocol;
    for (uint8_t note: {uint8_t{0}, uint8_t{60}, uint8_t{75}, uint8_t{127}}) {
        std::array<uint8_t, 512> buffer{};
        SeqPatternOpMessage op{SEQ_OP_SET_STEP_NOTE, 15, 63, note, 0, 0}, parsed_op;
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      buffer.data(), buffer.size(), MSG_SEQ_PATTERN_OP, &op, sizeof(op)),
                  0u);
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            buffer.data(), MSG_SEQ_PATTERN_OP, &parsed_op, sizeof(parsed_op)));
        EXPECT_EQ(parsed_op.arg_u8, note);
        SeqPatternSyncMessage original, parsed;
        original.steps[15].note = note;
        ASSERT_GT(
            ProtocolHandler::CreatePacket(
                buffer.data(), buffer.size(), MSG_SEQ_PATTERN_SYNC, &original, sizeof(original)),
            0u);
        ASSERT_TRUE(ProtocolHandler::ParseMessage(
            buffer.data(), MSG_SEQ_PATTERN_SYNC, &parsed, sizeof(parsed)));
        EXPECT_EQ(parsed.steps[15].note, note);
        EXPECT_EQ(sizeof(SeqPatternSyncMessage), 336u);
    }
}
