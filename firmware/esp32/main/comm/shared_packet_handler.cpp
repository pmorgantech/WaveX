#include "shared_packet_handler.h"

#include <string.h>

size_t SharedPacketHandler::create_control_change_packet(
    uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value) {
    return WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(
        buffer, buffer_size, parameter, channel, value);
}

size_t SharedPacketHandler::create_note_on_packet(
    uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel) {
    return WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(
        buffer, buffer_size, note, velocity, channel);
}

size_t SharedPacketHandler::create_note_off_packet(uint8_t* buffer,
                                                   size_t buffer_size,
                                                   uint8_t note,
                                                   uint8_t channel) {
    return WaveX::Protocol::ProtocolHandler::CreateNoteOffPacket(
        buffer, buffer_size, note, channel);
}

size_t SharedPacketHandler::create_sample_ctrl_packet(
    uint8_t* buffer, size_t buffer_size, uint8_t slot, uint8_t cmd, float rate) {
    WaveX::Protocol::SampleCtrlMessage msg;
    msg.slot = slot;
    msg.cmd = cmd;
    msg.rate = rate;

    return WaveX::Protocol::ProtocolHandler::CreateSampleCtrlPacket(buffer, buffer_size, msg);
}

size_t SharedPacketHandler::create_preview_req_packet(uint8_t* buffer,
                                                      size_t buffer_size,
                                                      uint8_t slot,
                                                      uint32_t start,
                                                      uint32_t end,
                                                      uint16_t decim) {
    WaveX::Protocol::PreviewReqMessage msg;
    msg.slot = slot;
    msg.start = start;
    msg.end = end;
    msg.decim = decim;

    return WaveX::Protocol::ProtocolHandler::CreatePreviewReqPacket(buffer, buffer_size, msg);
}

uint8_t SharedPacketHandler::calculate_checksum(const uint8_t* payload, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= payload[i];
    }
    return checksum;
}

bool SharedPacketHandler::validate_packet_header(const uint8_t* data, size_t length) {
    if (!data || length < 5)
        return false;

    // Check if it's a valid flexible packet type
    uint8_t packet_type = data[0];
    if (!WaveX::Protocol::ProtocolHandler::IsCommandPacketType(packet_type) &&
        !WaveX::Protocol::ProtocolHandler::IsDataPacketType(packet_type)) {
        return false;
    }

    // Get expected packet size
    size_t expected_size = WaveX::Protocol::ProtocolHandler::GetPacketSizeFromType(packet_type);
    if (expected_size == 0 || length != expected_size) {
        return false;
    }

    // Validate CRC
    return WaveX::Protocol::ProtocolHandler::ValidatePacketCrc(data, length);
}

uint8_t SharedPacketHandler::get_message_type(const uint8_t* data) {
    if (!data)
        return 0xFF;  // MSG_ERROR
    return data[1];
}

uint8_t SharedPacketHandler::get_payload_length(const uint8_t* data) {
    if (!data)
        return 0;
    return data[2];
}

const uint8_t* SharedPacketHandler::get_payload(const uint8_t* data) {
    if (!data)
        return NULL;
    return &data[4];  // Skip header (4 bytes)
}

size_t SharedPacketHandler::get_total_packet_size(const uint8_t* data) {
    if (!data)
        return 0;
    uint8_t payload_len = data[2];
    return 4 + payload_len + 1;  // header(4) + payload + checksum(1)
}
