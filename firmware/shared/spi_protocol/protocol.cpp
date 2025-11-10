#include "protocol.h"

#include <string.h>

#include <cstdio>

namespace WaveX {
namespace Protocol {

// Sequence number management - one per platform
static uint16_t s_next_seq_num = 1;  // Start from 1, 0 is reserved

// New simplified packet system functions - single unified format

// Get packet size from size code (4 bits)
size_t ProtocolHandler::GetPacketSizeFromCode(uint8_t size_code) {
    switch (size_code & PKT_SIZE_MASK) {
        case PKT_SIZE_32:
            return 32;
        case PKT_SIZE_64:
            return 64;
        case PKT_SIZE_128:
            return 128;
        case PKT_SIZE_256:
            return 256;
        case PKT_SIZE_512:
            return 512;
        case PKT_SIZE_1024:
            return 1024;
        case PKT_SIZE_2048:
            return 2048;
        default:
            return 0;  // Invalid size code
    }
}

// Get optimal size code for payload size
uint8_t ProtocolHandler::GetOptimalSizeCode(size_t payload_size) {
    if (payload_size <= 26)
        return PKT_SIZE_32;  // 32-4-2 = 26 bytes payload
    if (payload_size <= 58)
        return PKT_SIZE_64;  // 64-4-2 = 58 bytes payload
    if (payload_size <= 122)
        return PKT_SIZE_128;  // 128-4-2 = 122 bytes payload
    if (payload_size <= 250)
        return PKT_SIZE_256;  // 256-4-2 = 250 bytes payload
    if (payload_size <= 506)
        return PKT_SIZE_512;  // 512-4-2 = 506 bytes payload
    if (payload_size <= 1018)
        return PKT_SIZE_1024;  // 1024-4-2 = 1018 bytes payload
    if (payload_size <= 2042)
        return PKT_SIZE_2048;  // 2048-4-2 = 2042 bytes payload
    return PKT_SIZE_2048;      // Maximum 2048-byte packets (2048-4-2 = 2042 bytes payload)
}

// CRC16 calculation using CRC-16-CCITT to match Daisy hardware implementation
uint16_t ProtocolHandler::CalculateWaveXCrc(const uint8_t* data, size_t length) {
    if (!data || length == 0)
        return 0;

    // Use CRC-16-CCITT algorithm matching Daisy hardware CRC
    // Polynomial: 0x1021 (CRC-16-CCITT)
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)(data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

// Legacy CRC calculation functions (kept for compatibility)
uint16_t ProtocolHandler::CalculateSpiCrc(const uint8_t* data, size_t length) {
    return CalculateWaveXCrc(data, length);
}

uint16_t ProtocolHandler::CalculatePacketCrc(const uint8_t* packet_data, size_t packet_size) {
    // Calculate CRC over entire packet except last 2 bytes (CRC field)
    return CalculateWaveXCrc(packet_data, packet_size - sizeof(uint16_t));
}

// New simplified CRC validation
bool ProtocolHandler::ValidateWaveXPacket(const uint8_t* buffer, size_t buffer_size) {
    uint16_t calculated_crc = CalculateWaveXCrc(buffer, buffer_size - 2);
    uint16_t received_crc = buffer[buffer_size - 2] | (buffer[buffer_size - 1] << 8);
    return calculated_crc == received_crc;
}

// Core packet creation with automatic sequence number management
size_t ProtocolHandler::CreatePacket(uint8_t* buffer,
                                     size_t buffer_size,
                                     uint8_t msg_type,
                                     const void* payload,
                                     size_t payload_size,
                                     uint8_t flags) {
    // Get next sequence number and increment it
    uint16_t seq_num = s_next_seq_num++;

    // Use the existing CreateWaveXPacket method
    return CreateWaveXPacket(buffer, buffer_size, msg_type, payload, payload_size, seq_num, flags);
}

// Single packet creation function for all message types
size_t ProtocolHandler::CreateWaveXPacket(uint8_t* buffer,
                                          size_t buffer_size,
                                          uint8_t msg_type,
                                          const void* payload,
                                          size_t payload_size,
                                          uint16_t sequence_number,
                                          uint8_t flags) {
    // Determine optimal packet size based on payload size
    uint8_t size_code = GetOptimalSizeCode(payload_size);

    // Calculate total packet size
    size_t total_size = GetPacketSizeFromCode(size_code);
    if (total_size == 0 || total_size > buffer_size) {
        return 0;  // Invalid size or buffer too small
    }

    // Create packet header (4 bytes)
    buffer[0] = PKT_MAKE_FLAGS_SIZE(size_code, flags);  // flags + size
    buffer[1] = msg_type;                               // Message type
    buffer[2] = sequence_number & 0xFF;                 // Sequence number (low byte)
    buffer[3] = (sequence_number >> 8) & 0xFF;          // Sequence number (high byte)

    // Copy payload
    memcpy(buffer + 4, payload, payload_size);

    // Zero-pad remaining space
    memset(buffer + 4 + payload_size, 0, total_size - 4 - payload_size - 2);

    // Calculate CRC over entire packet except CRC field
    uint16_t crc = CalculateWaveXCrc(buffer, total_size - 2);
    buffer[total_size - 2] = crc & 0xFF;
    buffer[total_size - 1] = (crc >> 8) & 0xFF;

    return total_size;
}

// Single packet parsing function
bool ProtocolHandler::ParseWaveXPacket(const uint8_t* buffer,
                                       size_t buffer_size,
                                       uint8_t& msg_type,
                                       void* payload,
                                       size_t& payload_size,
                                       uint16_t& sequence_number,
                                       uint8_t& flags) {
    // Validate minimum packet size
    if (buffer_size < 6)
        return false;  // header(4) + crc(2)

    // Extract header information
    uint8_t size_code = buffer[0] & PKT_SIZE_MASK;
    flags = PKT_GET_FLAGS(buffer[0]);
    msg_type = buffer[1];
    sequence_number = buffer[2] | (buffer[3] << 8);  // 16-bit sequence number

    // Get actual packet size
    size_t total_size = GetPacketSizeFromCode(size_code);
    if (total_size != buffer_size)
        return false;

    // Validate CRC
    if (!ValidateWaveXPacket(buffer, buffer_size))
        return false;

    // Extract payload
    payload_size = total_size - 6;  // header(4) + crc(2)
    memcpy(payload, buffer + 4, payload_size);

    return true;
}

// Optimized CRC validation (legacy - kept for compatibility)
bool ProtocolHandler::ValidatePacketCrc(const uint8_t* packet_data, size_t packet_size) {
    uint16_t calculated_crc = CalculatePacketCrc(packet_data, packet_size);
    uint16_t received_crc = packet_data[packet_size - 2] | (packet_data[packet_size - 1] << 8);
    return calculated_crc == received_crc;
}

// Zero-pad unused packet areas
void ProtocolHandler::ZeroPadPacket(uint8_t* packet_data, size_t packet_size, size_t used_size) {
    if (used_size < packet_size) {
        memset(packet_data + used_size, 0, packet_size - used_size);
    }
}

// Simplified packet creation functions using new unified format

// Generic packet creation function - DRY principle
static size_t CreateUnifiedPacket(uint8_t* buffer,
                                  size_t buffer_size,
                                  uint8_t msg_type,
                                  const void* payload_data,
                                  size_t payload_size,
                                  uint8_t flags = 0) {
    return ProtocolHandler::CreatePacket(
        buffer, buffer_size, msg_type, payload_data, payload_size, flags);
}

// Create error packet using unified packet system
size_t ProtocolHandler::CreateErrorPacket(uint8_t* buffer,
                                          size_t buffer_size,
                                          const ErrorMessage& err) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_ERROR, &err, sizeof(ErrorMessage));
}

// Create sample status packet using unified packet system
size_t ProtocolHandler::CreateSampleStatusPacket(uint8_t* buffer,
                                                 size_t buffer_size,
                                                 const SampleStatusMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_STATUS, &msg, sizeof(SampleStatusMessage));
}

// Create sample stop response packet using unified packet system
size_t ProtocolHandler::CreateSampleStopRespPacket(uint8_t* buffer,
                                                   size_t buffer_size,
                                                   const SampleStopRespMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_STOP_RESP, &msg, sizeof(SampleStopRespMessage));
}

// Create sample stop request packet using unified packet system
size_t ProtocolHandler::CreateSampleStopReqPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const SampleStopReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_STOP_REQ, &msg, sizeof(SampleStopReqMessage));
}

// Create browse response packet using unified packet system
size_t ProtocolHandler::CreateBrowseRespPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               uint32_t total_count,
                                               const FileEntryWire* entries,
                                               uint8_t n) {
    // Calculate payload size: total_count (4 bytes) + n_entries (1 byte) + entries array
    size_t payload_size = sizeof(uint32_t) + sizeof(uint8_t) + (size_t)n * sizeof(FileEntryWire);

    // Create temporary payload buffer - increased size to handle max entries
    uint8_t temp_payload[2048];  // Increased from 1024 to 2048 bytes to handle 20+ entries
    if (payload_size > sizeof(temp_payload)) {
        return 0;  // Payload too large
    }

    // Copy total_count first
    memcpy(temp_payload, &total_count, sizeof(uint32_t));

    // Copy n_entries count
    temp_payload[sizeof(uint32_t)] = n;

    // Copy entries array
    if (n > 0 && entries != NULL) {
        memcpy(temp_payload + sizeof(uint32_t) + sizeof(uint8_t),
               entries,
               (size_t)n * sizeof(FileEntryWire));
    }

    return CreateUnifiedPacket(buffer, buffer_size, MSG_BROWSE_RESP, temp_payload, payload_size);
}

// Create sample path response packet using unified packet system
size_t ProtocolHandler::CreateSamplePathResponsePacket(uint8_t* buffer,
                                                       size_t buffer_size,
                                                       const SamplePathResponseMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_GET_PATH_RESP, &msg, sizeof(SamplePathResponseMessage));
}

// Create control change packet using unified packet system
size_t ProtocolHandler::CreateControlChangePacket(
    uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value) {
    ControlChangeMessage msg;
    msg.parameter = parameter;
    msg.channel = channel;
    msg.value = value;
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_CONTROL_CHANGE, &msg, sizeof(ControlChangeMessage));
}

// Create note on packet using unified packet system
size_t ProtocolHandler::CreateNoteOnPacket(
    uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel) {
    NoteMessage msg;
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = channel;
    msg.reserved = 0;
    return CreateUnifiedPacket(buffer, buffer_size, MSG_NOTE_ON, &msg, sizeof(NoteMessage));
}

// Create note off packet using unified packet system
size_t ProtocolHandler::CreateNoteOffPacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            uint8_t note,
                                            uint8_t channel) {
    NoteMessage msg;
    msg.note = note;
    msg.velocity = 0;
    msg.channel = channel;
    msg.reserved = 0;
    return CreateUnifiedPacket(buffer, buffer_size, MSG_NOTE_OFF, &msg, sizeof(NoteMessage));
}

// Create sample control packet using unified packet system
size_t ProtocolHandler::CreateSampleCtrlPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               const SampleCtrlMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_CTRL, &msg, sizeof(SampleCtrlMessage));
}

// Create preview request packet using unified packet system
size_t ProtocolHandler::CreatePreviewReqPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               const PreviewReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_PREVIEW_REQ, &msg, sizeof(PreviewReqMessage));
}

// Create data request packet using unified packet system
size_t ProtocolHandler::CreateDataRequestPacket(uint8_t* buffer,
                                                size_t buffer_size,
                                                const DataRequestMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_DATA_REQUEST, &msg, sizeof(DataRequestMessage));
}

// Create meter push packet using unified packet system
size_t ProtocolHandler::CreateMeterPushPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const MeterPushMessage& msg) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_METER_PUSH, &msg, sizeof(MeterPushMessage));
}

// Create sync packet using unified packet system - force 32-byte packets for SPI
size_t ProtocolHandler::CreateSyncPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         const SyncMessage& msg) {
    return CreatePacket(buffer, buffer_size, MSG_SYNC, &msg, sizeof(SyncMessage), 0);
}

// Create heartbeat packet using unified packet system - force 32-byte packets for SPI
size_t ProtocolHandler::CreateHeartbeatPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const HeartbeatMessage& msg) {
    return CreatePacket(buffer, buffer_size, MSG_HEARTBEAT, &msg, sizeof(HeartbeatMessage), 0);
}

// Create ACK packet using unified packet system
size_t ProtocolHandler::CreateAckPacket(uint8_t* buffer,
                                        size_t buffer_size,
                                        const AckMessage& ack) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_ACK, &ack, sizeof(AckMessage));
}

// Create sample play index packet using unified packet system
size_t ProtocolHandler::CreateSamplePlayIndexPacket(uint8_t* buffer,
                                                    size_t buffer_size,
                                                    const SamplePlayIndexMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_PLAY_INDEX_REQ, &msg, sizeof(SamplePlayIndexMessage));
}

// Create sample get path packet using unified packet system
size_t ProtocolHandler::CreateSampleGetPathPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const SampleGetPathMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_GET_PATH_REQ, &msg, sizeof(SampleGetPathMessage));
}

// Create wave chunk packet using unified packet system
size_t ProtocolHandler::CreateWaveChunkPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const WaveChunkMessage& msg,
                                              const void* sample_data,
                                              size_t sample_data_size) {
    // Calculate total payload size: header + sample data
    size_t header_size = sizeof(WaveChunkMessage);
    size_t total_payload_size = header_size + sample_data_size;

    // Create temporary payload buffer
    uint8_t temp_payload[2048];
    if (total_payload_size > sizeof(temp_payload)) {
        return 0;  // Payload too large
    }

    // Copy header
    memcpy(temp_payload, &msg, header_size);

    // Copy sample data
    if (sample_data && sample_data_size > 0) {
        memcpy(temp_payload + header_size, sample_data, sample_data_size);
    }

    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_WAVE_CHUNK, temp_payload, total_payload_size);
}

// Legacy parsing functions - simplified for new unified format
bool ProtocolHandler::ParseBrowseReq(const uint8_t* buffer,
                                     char* path_out,
                                     size_t path_max,
                                     uint32_t& start_index,
                                     uint8_t& max_entries) {
    if (buffer == NULL || path_out == NULL) {
        return false;
    }

    // For browse request, parse the payload directly
    // Format: path (null-terminated) + start_index (4 bytes) + max_entries (1 byte)
    size_t path_len = strlen((const char*)buffer);
    if (path_len >= path_max) {
        return false;  // Path too long
    }

    // Extract path
    strncpy(path_out, (const char*)buffer, path_max - 1);
    path_out[path_max - 1] = '\0';

    // Extract start_index and max_entries
    const uint8_t* data_ptr = buffer + path_len + 1;
    if (data_ptr + sizeof(uint32_t) + sizeof(uint8_t) > buffer + 1024) {  // Safety check
        return false;
    }

    start_index = *(const uint32_t*)data_ptr;
    data_ptr += sizeof(uint32_t);
    max_entries = *data_ptr;

    return true;
}

bool ProtocolHandler::ParseSamplePlayReq(const uint8_t* buffer, char* path_out, size_t path_max) {
    if (buffer == NULL || path_out == NULL) {
        return false;
    }

    // Parse sample play request payload: path (null-terminated)
    strncpy(path_out, (const char*)buffer, path_max - 1);
    path_out[path_max - 1] = '\0';
    return true;
}

// Compatibility wrapper functions for tests
bool ProtocolHandler::ValidatePacket(const uint8_t* buffer, size_t length) {
    return ValidateWaveXPacket(buffer, length);
}

MessageType ProtocolHandler::GetMessageType(const uint8_t* buffer) {
    if (!buffer)
        return MSG_ERROR;
    size_t packet_size = GetPacketSize(buffer);
    if (packet_size < 2)
        return MSG_ERROR;
    return static_cast<MessageType>(buffer[1]);
}

bool ProtocolHandler::ParseControlChange(const uint8_t* buffer, ControlChangeMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(ControlChangeMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_CONTROL_CHANGE;
}

bool ProtocolHandler::ParseNoteMessage(const uint8_t* buffer, NoteMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(NoteMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return (msg_type == MSG_NOTE_ON || msg_type == MSG_NOTE_OFF);
}

bool ProtocolHandler::ParseSampleCtrl(const uint8_t* buffer, SampleCtrlMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(SampleCtrlMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_SAMPLE_CTRL;
}

bool ProtocolHandler::ParsePreviewReq(const uint8_t* buffer, PreviewReqMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(PreviewReqMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_PREVIEW_REQ;
}

bool ProtocolHandler::ParseDataRequest(const uint8_t* buffer, DataRequestMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(DataRequestMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_DATA_REQUEST;
}

bool ProtocolHandler::ParseMessage(const uint8_t* buffer, HeartbeatMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(HeartbeatMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_HEARTBEAT;
}

bool ProtocolHandler::ParseMessage(const uint8_t* buffer,
                                   MessageType expected_type,
                                   void* out_payload,
                                   size_t out_payload_size) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = out_payload_size;
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, out_payload, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == static_cast<uint8_t>(expected_type);
}

size_t ProtocolHandler::GetPacketSize(const uint8_t* buffer) {
    if (!buffer)
        return 0;
    uint8_t size_code = buffer[0] & PKT_SIZE_MASK;
    return GetPacketSizeFromCode(size_code);
}

uint8_t ProtocolHandler::CalculateChecksum(const uint8_t* data, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

}  // namespace Protocol
}  // namespace WaveX
