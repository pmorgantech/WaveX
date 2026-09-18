#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
TEST(BankProtocol, OperationsAndRetainedResultsRoundTrip) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op = BANK_GET; op <= BANK_PRELOAD; ++op) {
        BankOpMessage request;
        request.request_id = 123;
        request.revision = 456;
        request.op = op;
        request.slot = 127;
        request.track = 15;
        request.flags = BANK_CONFIRM_REPLACE;
        std::strcpy(request.name, "Live set");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_BANK_OP, &request, sizeof(request)),
                  0u);
        BankOpMessage read;
        ASSERT_TRUE(ProtocolHandler::ParseMessage(packet.data(), MSG_BANK_OP, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&read, &request, sizeof(read)), 0);
        EXPECT_TRUE(IsValidBankOp(read));
    }
    for (uint8_t error = BANK_OK; error <= BANK_BAD_SLOT; ++error) {
        BankStatusMessage status;
        status.request_id = 1;
        status.active_request_id = 2;
        status.completed_request_id = 3;
        status.busy = 1;
        status.active_op = BANK_STORE_COPY;
        status.completed_op = BANK_PRELOAD;
        status.error = error;
        status.loaded = status.occupied = 1;
        status.slot = 127;
        std::strcpy(status.name, "Night");
        std::strcpy(status.instrument, "Bass");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_BANK_STATUS, &status, sizeof(status)),
                  0u);
        BankStatusMessage read;
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_BANK_STATUS, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&read, &status, sizeof(read)), 0);
        EXPECT_TRUE(IsValidBankStatus(read));
    }
}
TEST(BankProtocol, RejectsInvalidBoundsFlagsAndStatus) {
    BankOpMessage op;
    EXPECT_FALSE(IsValidBankOp(op));
    op.request_id = 1;
    EXPECT_TRUE(IsValidBankOp(op));
    op.slot = 128;
    EXPECT_FALSE(IsValidBankOp(op));
    op.slot = 0;
    op.track = 16;
    EXPECT_FALSE(IsValidBankOp(op));
    op.track = 0;
    op.flags = 2;
    EXPECT_FALSE(IsValidBankOp(op));
    op.flags = 0;
    op.op = BANK_PRELOAD + 1;
    EXPECT_FALSE(IsValidBankOp(op));
    BankStatusMessage status;
    status.request_id = 1;
    EXPECT_TRUE(IsValidBankStatus(status));
    status.occupied = 1;
    EXPECT_FALSE(IsValidBankStatus(status));
    status.loaded = 1;
    status.busy = 1;
    EXPECT_FALSE(IsValidBankStatus(status));
    status.active_request_id = 2;
    status.active_op = BANK_RECALL;
    EXPECT_TRUE(IsValidBankStatus(status));
    std::memset(status.instrument, 'x', sizeof(status.instrument));
    EXPECT_FALSE(IsValidBankStatus(status));
}

TEST(BankProtocol, MidiProgramRoundTripAndBounds) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t program: {uint8_t{0}, uint8_t{127}}) {
        MidiProgramMessage request(program, 15), read;
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_MIDI_PROGRAM, &request, sizeof(request)),
                  0u);
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_MIDI_PROGRAM, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&read, &request, sizeof(read)), 0);
        EXPECT_TRUE(IsValidMidiProgram(read));
    }
    MidiProgramMessage invalid(128, 0);
    EXPECT_FALSE(IsValidMidiProgram(invalid));
    invalid = MidiProgramMessage(0, 16);
    EXPECT_FALSE(IsValidMidiProgram(invalid));
    invalid = MidiProgramMessage(0, 0);
    invalid.reserved = 1;
    EXPECT_FALSE(IsValidMidiProgram(invalid));
    BankStatusMessage status;
    status.request_id = status.completed_request_id = 1;
    status.completed_op = BANK_PROGRAM_RECALL;
    EXPECT_TRUE(IsValidBankStatus(status));
    BankOpMessage op;
    op.request_id = 1;
    op.op = BANK_PROGRAM_RECALL;  // UI must not bypass recall confirmation
    EXPECT_FALSE(IsValidBankOp(op));
}

TEST(BankProtocol, SlotTransfersRoundTripAndRejectInvalidFields) {
    std::array<uint8_t, 128> packet{};
    for (uint8_t op: {BANK_COPY_SLOT, BANK_MOVE_SLOT}) {
        BankSlotOpMessage request, read;
        request.request_id = 123;
        request.revision = 456;
        request.op = op;
        request.source_slot = 127;
        request.destination_slot = 0;
        request.flags = BANK_CONFIRM_REPLACE;
        std::strcpy(request.name, "Reordered kit");
        ASSERT_GT(ProtocolHandler::CreatePacket(
                      packet.data(), packet.size(), MSG_BANK_SLOT_OP, &request, sizeof(request)),
                  0u);
        ASSERT_TRUE(
            ProtocolHandler::ParseMessage(packet.data(), MSG_BANK_SLOT_OP, &read, sizeof(read)));
        EXPECT_EQ(std::memcmp(&read, &request, sizeof(read)), 0);
        EXPECT_TRUE(IsValidBankSlotOp(read));
        BankStatusMessage status;
        status.request_id = status.completed_request_id = 1;
        status.completed_op = op;
        EXPECT_TRUE(IsValidBankStatus(status));
    }
    BankSlotOpMessage request;
    EXPECT_FALSE(IsValidBankSlotOp(request));
    request.request_id = 1;
    EXPECT_TRUE(IsValidBankSlotOp(request));
    request.source_slot = 128;
    EXPECT_FALSE(IsValidBankSlotOp(request));
    request.source_slot = 0;
    request.destination_slot = 128;
    EXPECT_FALSE(IsValidBankSlotOp(request));
    request.destination_slot = 1;
    request.flags = 2;
    EXPECT_FALSE(IsValidBankSlotOp(request));
    request.flags = 0;
    request.op = BANK_PROGRAM_RECALL;
    EXPECT_FALSE(IsValidBankSlotOp(request));
    request.op = BANK_MOVE_SLOT + 1;
    EXPECT_FALSE(IsValidBankSlotOp(request));
}
