#include "protocol.h"
#include <string.h>
#include <cstdio>

namespace WaveX {
namespace Protocol {

// Internal helper to build a packet with given type and payload
static size_t BuildPacket(uint8_t* buffer, size_t buffer_size,
                          uint8_t type, const void* payload, size_t payload_size)
{
    if (buffer_size < sizeof(PacketHeader) + payload_size) {
        return 0;
    }

    Packet* packet = reinterpret_cast<Packet*>(buffer);
    packet->header.sync = SYNC_BYTE;
    packet->header.type = type;
    // Check if payload size fits in uint8_t
    if (payload_size > 255) {
        return 0; // Payload too large for PacketHeader format
    }
    packet->header.length = static_cast<uint8_t>(payload_size);

    if (payload_size > 0 && payload != NULL) {
        memcpy(packet->payload, payload, payload_size);
    }

    packet->header.checksum = ProtocolHandler::CalculateChecksum(packet->payload, packet->header.length);
    return sizeof(PacketHeader) + packet->header.length;
}

size_t ProtocolHandler::CreateControlChangePacket(uint8_t* buffer, size_t buffer_size,
                                                uint8_t parameter, uint8_t channel, uint16_t value)
{
    ControlChangeMessage msg; memset(&msg, 0, sizeof(msg));
    msg.parameter = parameter;
    msg.channel = channel;
    msg.value = value;
    return BuildPacket(buffer, buffer_size, MSG_CONTROL_CHANGE, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateNoteOnPacket(uint8_t* buffer, size_t buffer_size,
                                         uint8_t note, uint8_t velocity, uint8_t channel)
{
    NoteMessage msg; memset(&msg, 0, sizeof(msg));
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = channel;
    msg.reserved = 0;
    return BuildPacket(buffer, buffer_size, MSG_NOTE_ON, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateNoteOffPacket(uint8_t* buffer, size_t buffer_size,
                                          uint8_t note, uint8_t channel)
{
    NoteMessage msg; memset(&msg, 0, sizeof(msg));
    msg.note = note;
    msg.velocity = 0;
    msg.channel = channel;
    msg.reserved = 0;
    return BuildPacket(buffer, buffer_size, MSG_NOTE_OFF, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateSampleCtrlPacket(uint8_t* buffer, size_t buffer_size,
                                             const SampleCtrlMessage& msg)
{
    return BuildPacket(buffer, buffer_size, MSG_SAMPLE_CTRL, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreatePreviewReqPacket(uint8_t* buffer, size_t buffer_size,
                                             const PreviewReqMessage& msg)
{
    return BuildPacket(buffer, buffer_size, MSG_PREVIEW_REQ, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateDataRequestPacket(uint8_t* buffer, size_t buffer_size,
                                              const DataRequestMessage& msg)
{
    return BuildPacket(buffer, buffer_size, MSG_DATA_REQUEST, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateMeterPushPacket(uint8_t* buffer, size_t buffer_size,
                                            const MeterPushMessage& msg)
{
    return BuildPacket(buffer, buffer_size, MSG_METER_PUSH, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateWaveChunkPacket(uint8_t* buffer, size_t buffer_size,
                                            const WaveChunkMessage& msg, const void* sample_data, size_t sample_data_size)
{
    // Create a combined payload: header + sample data
    if (buffer_size < sizeof(PacketHeader) + sizeof(WaveChunkMessage) + sample_data_size) {
        return 0;
    }
    
    // First, create the wave chunk message payload
    uint8_t combined_payload[sizeof(WaveChunkMessage) + MAX_PAYLOAD_SIZE];
    memcpy(combined_payload, &msg, sizeof(WaveChunkMessage));
    if (sample_data && sample_data_size > 0) {
        memcpy(combined_payload + sizeof(WaveChunkMessage), sample_data, sample_data_size);
    }
    
    return BuildPacket(buffer, buffer_size, MSG_WAVE_CHUNK, combined_payload, 
                      sizeof(WaveChunkMessage) + sample_data_size);
}

size_t ProtocolHandler::CreateGenericPacket(uint8_t* buffer, size_t buffer_size,
                                          uint8_t type, const void* payload, size_t payload_size)
{
    return BuildPacket(buffer, buffer_size, type, payload, payload_size);
}

size_t ProtocolHandler::CreateHeartbeatPacket(uint8_t* buffer, size_t buffer_size,
                                            const HeartbeatMessage& msg)
{
    return BuildPacket(buffer, buffer_size, MSG_HEARTBEAT, &msg, sizeof(msg));
}

bool ProtocolHandler::ParseBrowseReq(const uint8_t* buffer, char* path_out, size_t path_max,
                                     uint32_t& start_index, uint8_t& max_entries)
{
    if(!buffer || !path_out || path_max == 0) return false;
    const Packet* p = reinterpret_cast<const Packet*>(buffer);
    if(p->header.type != MSG_BROWSE_REQ) return false;
    // Payload layout: path (null-terminated), start_index(u32), max_entries(u8)
    const uint8_t* pay = p->payload;
    size_t len = p->header.length;
    // Find NUL within bounds
    size_t i = 0;
    while(i < len && pay[i] != 0) i++;
    if(i >= len) return false;
    size_t path_len = i + 1; // include NUL
    size_t need = path_len + sizeof(uint32_t) + sizeof(uint8_t);
    if(len < need) return false;
    size_t copy = (path_len > path_max) ? (path_max - 1) : (path_len - 1);
    memcpy(path_out, pay, copy);
    path_out[copy] = 0;
    const uint8_t* pidx = pay + path_len;
    start_index = (uint32_t)pidx[0] | ((uint32_t)pidx[1] << 8) | ((uint32_t)pidx[2] << 16) | ((uint32_t)pidx[3] << 24);
    max_entries = *(pidx + 4);
    return true;
}

size_t ProtocolHandler::CreateBrowseRespPacket(uint8_t* buffer, size_t buffer_size,
                                               uint32_t total_count,
                                               const FileEntryWire* entries,
                                               uint8_t n)
{
    if(n == 0 || !entries) n = 0;
    size_t payload_size = sizeof(BrowseRespHeader) + (size_t)n * sizeof(FileEntryWire);
    if(buffer_size < sizeof(PacketHeader) + payload_size) return 0;
    uint8_t tmp[600];  // Increased to accommodate ~10 file entries (53 bytes each + header)
    if(payload_size > sizeof(tmp)) return 0;
    BrowseRespHeader hdr;
    hdr.total_count = total_count;
    hdr.n = n;
    memcpy(tmp, &hdr, sizeof(hdr));
    if(n) memcpy(tmp + sizeof(hdr), entries, (size_t)n * sizeof(FileEntryWire));
    return BuildPacket(buffer, buffer_size, MSG_BROWSE_RESP, tmp, payload_size);
}

bool ProtocolHandler::ParseSamplePlayReq(const uint8_t* buffer, char* path_out, size_t path_max)
{
    if(!buffer || !path_out || path_max == 0) return false;
    const Packet* p = reinterpret_cast<const Packet*>(buffer);
    if(p->header.type != MSG_SAMPLE_PLAY_REQ) return false;
    size_t len = p->header.length;
    size_t copy = (len >= path_max) ? (path_max - 1) : len;
    memcpy(path_out, p->payload, copy);
    path_out[copy] = 0;
    return true;
}

size_t ProtocolHandler::CreateSampleStatusPacket(uint8_t* buffer, size_t buffer_size,
                                                 const SampleStatusMessage& msg)
{
    return BuildPacket(buffer, buffer_size, MSG_SAMPLE_STATUS, &msg, sizeof(msg));
}

size_t ProtocolHandler::CreateErrorPacket(uint8_t* buffer, size_t buffer_size,
                                          const ErrorMessage& err)
{
    return BuildPacket(buffer, buffer_size, MSG_ERROR, &err, sizeof(err));
}

bool ProtocolHandler::ValidatePacket(const uint8_t* buffer, size_t length)
{
    if (length < sizeof(PacketHeader)) {
        return false;
    }
    
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    
    // Check sync byte
    if (packet->header.sync != SYNC_BYTE) {
        return false;
    }
    
    // Check packet length
    if (length < sizeof(PacketHeader) + packet->header.length) {
        return false;
    }
    
    // Verify checksum
    uint8_t calculated_checksum = CalculateChecksum(packet->payload, packet->header.length);
    if (packet->header.checksum != calculated_checksum) {
        return false;
    }
    
    return true;
}

MessageType ProtocolHandler::GetMessageType(const uint8_t* buffer)
{
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    return static_cast<MessageType>(packet->header.type);
}

bool ProtocolHandler::ParseControlChange(const uint8_t* buffer, ControlChangeMessage& msg)
{
    return ParseMessage(buffer, MSG_CONTROL_CHANGE, &msg, sizeof(msg));
}

bool ProtocolHandler::ParseNoteMessage(const uint8_t* buffer, NoteMessage& msg)
{
    // Allow either NOTE_ON or NOTE_OFF
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    if (packet->header.length != sizeof(NoteMessage)) return false;
    if (packet->header.type != MSG_NOTE_ON && packet->header.type != MSG_NOTE_OFF) return false;
    memcpy(&msg, packet->payload, sizeof(NoteMessage));
    return true;
}

bool ProtocolHandler::ParseSampleCtrl(const uint8_t* buffer, SampleCtrlMessage& msg)
{
    return ParseMessage(buffer, MSG_SAMPLE_CTRL, &msg, sizeof(msg));
}

bool ProtocolHandler::ParsePreviewReq(const uint8_t* buffer, PreviewReqMessage& msg)
{
    return ParseMessage(buffer, MSG_PREVIEW_REQ, &msg, sizeof(msg));
}

bool ProtocolHandler::ParseDataRequest(const uint8_t* buffer, DataRequestMessage& msg)
{
    return ParseMessage(buffer, MSG_DATA_REQUEST, &msg, sizeof(msg));
}

bool ProtocolHandler::ParseMessage(const uint8_t* buffer, HeartbeatMessage& msg)
{
    return ParseMessage(buffer, MSG_HEARTBEAT, &msg, sizeof(msg));
}

bool ProtocolHandler::ParseMessage(const uint8_t* buffer, MessageType expected_type, void* out_payload, size_t out_payload_size)
{
    if (!buffer || !out_payload || out_payload_size == 0) return false;
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    if (packet->header.type != expected_type) return false;
    if (packet->header.length != out_payload_size) return false;
    memcpy(out_payload, packet->payload, out_payload_size);
    return true;
}

uint8_t ProtocolHandler::CalculateChecksum(const uint8_t* data, size_t length)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

size_t ProtocolHandler::GetPacketSize(const uint8_t* buffer)
{
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    return sizeof(PacketHeader) + packet->header.length;
}

size_t ProtocolHandler::CreateAckPacket(uint8_t* buffer, size_t buffer_size,
                                       const AckMessage& ack)
{
    return BuildPacket(buffer, buffer_size, MSG_ACK, &ack, sizeof(ack));
}

// SPI packet functions
bool ProtocolHandler::ValidateSpiPacket(const uint8_t* data, size_t size)
{
    if (!data || size < 8) {
        // Debug: Log validation failure
        printf("ValidateSpiPacket: Failed basic checks - data=%p, size=%zu\n", data, size);
        return false;
    }
    
    uint8_t type = data[0];
    uint8_t len = data[3];
    
    // Debug: Log packet details
    printf("ValidateSpiPacket: type=0x%02X, len=%d, size=%zu\n", type, len, size);
    printf("ValidateSpiPacket: First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
    printf("ValidateSpiPacket: SPI_CMD_PKT_SIZE=%zu, SPI_DATA_PKT_SIZE=%zu\n", 
           SPI_CMD_PKT_SIZE, SPI_DATA_PKT_SIZE);
    
    // Validate packet type
    if (type != 0x01 && type != 0x02) {
        printf("ValidateSpiPacket: Invalid packet type 0x%02X\n", type);
        return false;
    }
    
    // Validate length based on packet type
    if (type == 0x01) { // Command packet
        if (len > 20 || size < SPI_CMD_PKT_SIZE) {
            printf("ValidateSpiPacket: Command packet validation failed - len=%d, size=%zu, SPI_CMD_PKT_SIZE=%zu\n", 
                   len, size, SPI_CMD_PKT_SIZE);
            return false;
        }
    } else if (type == 0x02) { // Data packet
        if (len > 240 || size < SPI_DATA_PKT_SIZE) {
            printf("ValidateSpiPacket: Data packet validation failed - len=%d, size=%zu, SPI_DATA_PKT_SIZE=%zu\n", 
                   len, size, SPI_DATA_PKT_SIZE);
            return false;
        }
    }
    
    // Validate CRC (CRC is at fixed offset 4)
    // Calculate CRC over header + payload (excluding CRC field itself)
    uint8_t crc_data[4 + 20]; // Header (4) + max payload (20)
    crc_data[0] = data[0]; // type
    crc_data[1] = data[1]; // flags
    crc_data[2] = data[2]; // seq
    crc_data[3] = data[3]; // len
    memcpy(&crc_data[4], &data[6], len); // payload (starts at offset 6)
    uint16_t calculated_crc = CalculateSpiCrc(crc_data, 4 + len);
    uint16_t received_crc = data[4] | (data[5] << 8);
    
    printf("ValidateSpiPacket: CRC check - calculated=0x%04X, received=0x%04X, len=%d\n", 
           calculated_crc, received_crc, len);
    
    bool crc_valid = (calculated_crc == received_crc);
    if (!crc_valid) {
        printf("ValidateSpiPacket: CRC validation failed\n");
    }
    
    return crc_valid;
}

size_t ProtocolHandler::GetSpiPacketSize(const uint8_t* data)
{
    if (!data) return 0;
    
    uint8_t type = data[0];
    
    // Command packets: type=0x01, length in byte 3
    if (type == 0x01) {
        return SPI_CMD_PKT_SIZE;
    }
    
    // Data packets: type=0x02, length in byte 3  
    if (type == 0x02) {
        return SPI_DATA_PKT_SIZE;
    }
    
    // If we get here, it's likely a data packet with unknown type
    // Check if it looks like a data packet by examining the structure
    if (data[1] == 0x00 && data[2] == 0x00 && data[3] <= 240) {
        // Looks like a data packet structure
        return SPI_DATA_PKT_SIZE;
    }
    
    // Default to command packet size for unknown formats
    return SPI_CMD_PKT_SIZE;
}

uint16_t ProtocolHandler::CalculateSpiCrc(const uint8_t* data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

bool ProtocolHandler::IsCommandPacket(const uint8_t* data)
{
    return data && data[0] == 0x01;
}

bool ProtocolHandler::IsDataPacket(const uint8_t* data)
{
    return data && data[0] == 0x02;
}

} // namespace Protocol
} // namespace WaveX 