#include "shared_packet_handler.h"
#include <string.h>

size_t SharedPacketHandler::create_control_change_packet(uint8_t* buffer, size_t buffer_size, 
                                                       uint8_t parameter, uint8_t channel, uint16_t value)
{
    if (buffer_size < 8) return 0; // Need at least 8 bytes: header(4) + payload(3) + checksum(1)
    
    WaveX::Protocol::ControlChangeMessage msg;
    msg.parameter = parameter;
    msg.channel = channel;
    msg.value = value;
    
    return create_generic_packet(buffer, buffer_size, WaveX::Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
}

size_t SharedPacketHandler::create_note_on_packet(uint8_t* buffer, size_t buffer_size, 
                                                uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (buffer_size < 8) return 0; // Need at least 8 bytes: header(4) + payload(3) + checksum(1)
    
    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = channel;
    msg.reserved = 0;
    
    return create_generic_packet(buffer, buffer_size, WaveX::Protocol::MSG_NOTE_ON, &msg, sizeof(msg));
}

size_t SharedPacketHandler::create_note_off_packet(uint8_t* buffer, size_t buffer_size, 
                                                 uint8_t note, uint8_t channel)
{
    if (buffer_size < 8) return 0; // Need at least 8 bytes: header(4) + payload(3) + checksum(1)
    
    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = 0; // Note off
    msg.channel = channel;
    msg.reserved = 0;
    
    return create_generic_packet(buffer, buffer_size, WaveX::Protocol::MSG_NOTE_OFF, &msg, sizeof(msg));
}

size_t SharedPacketHandler::create_sample_ctrl_packet(uint8_t* buffer, size_t buffer_size, 
                                                    uint8_t slot, uint8_t cmd, float rate)
{
    if (buffer_size < 12) return 0; // Need at least 12 bytes: header(4) + payload(7) + checksum(1)
    
    WaveX::Protocol::SampleCtrlMessage msg;
    msg.slot = slot;
    msg.cmd = cmd;
    msg.rate = rate;
    
    return create_generic_packet(buffer, buffer_size, WaveX::Protocol::MSG_SAMPLE_CTRL, &msg, sizeof(msg));
}

size_t SharedPacketHandler::create_preview_req_packet(uint8_t* buffer, size_t buffer_size, 
                                                    uint8_t slot, uint32_t start, uint32_t end, uint16_t decim)
{
    if (buffer_size < 16) return 0; // Need at least 16 bytes: header(4) + payload(11) + checksum(1)
    
    WaveX::Protocol::PreviewReqMessage msg;
    msg.slot = slot;
    msg.start = start;
    msg.end = end;
    msg.decim = decim;
    
    return create_generic_packet(buffer, buffer_size, WaveX::Protocol::MSG_PREVIEW_REQ, &msg, sizeof(msg));
}

size_t SharedPacketHandler::create_generic_packet(uint8_t* buffer, size_t buffer_size, 
                                                uint8_t msg_type, const void* payload, size_t payload_size)
{
    if (buffer_size < 4 + payload_size + 1) return 0; // header(4) + payload + checksum(1)
    
    size_t pos = 0;
    
    // SYNC byte
    buffer[pos++] = WaveX::Protocol::SYNC_BYTE;
    
    // Message type
    buffer[pos++] = msg_type;
    
    // Payload length
    buffer[pos++] = (uint8_t)payload_size;
    
    // Copy payload
    if (payload && payload_size > 0) {
        memcpy(&buffer[pos], payload, payload_size);
        pos += payload_size;
    }
    
    // Calculate and add checksum
    uint8_t checksum = calculate_checksum(&buffer[4], payload_size);
    buffer[pos++] = checksum;
    
    return pos;
}

uint8_t SharedPacketHandler::calculate_checksum(const uint8_t* payload, size_t length)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= payload[i];
    }
    return checksum;
}

bool SharedPacketHandler::validate_packet_header(const uint8_t* data, size_t length)
{
    if (length < 4) return false;
    
    // Check sync byte
    if (data[0] != WaveX::Protocol::SYNC_BYTE) return false;
    
    // Check payload length consistency
    uint8_t payload_len = data[2];
    if (length != 4 + payload_len + 1) return false; // header(4) + payload + checksum(1)
    
    // Validate checksum
    uint8_t expected_checksum = calculate_checksum(&data[4], payload_len);
    return data[3 + payload_len] == expected_checksum;
}

uint8_t SharedPacketHandler::get_message_type(const uint8_t* data)
{
    if (!data) return 0xFF; // MSG_ERROR
    return data[1];
}

uint8_t SharedPacketHandler::get_payload_length(const uint8_t* data)
{
    if (!data) return 0;
    return data[2];
}

const uint8_t* SharedPacketHandler::get_payload(const uint8_t* data)
{
    if (!data) return NULL;
    return &data[4]; // Skip header (4 bytes)
}

size_t SharedPacketHandler::get_total_packet_size(const uint8_t* data)
{
    if (!data) return 0;
    uint8_t payload_len = data[2];
    return 4 + payload_len + 1; // header(4) + payload + checksum(1)
}
