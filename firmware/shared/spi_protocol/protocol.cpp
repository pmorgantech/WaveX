#include "protocol.h"
#include <string.h>
#include <cstdio>

namespace WaveX {
namespace Protocol {

// Legacy packet creation functions removed - using flexible packet system only

// Flexible packet system functions

// Packet size lookup table
size_t ProtocolHandler::GetPacketSizeFromType(uint8_t packet_type) {
    switch (packet_type) {
        // Command packets
        case PKT_TYPE_CMD32:    return PKT_SIZE_32;
        case PKT_TYPE_CMD64:    return PKT_SIZE_64;
        case PKT_TYPE_CMD128:   return PKT_SIZE_128;
        case PKT_TYPE_CMD256:   return PKT_SIZE_256;
        case PKT_TYPE_CMD512:   return PKT_SIZE_512;
        case PKT_TYPE_CMD1024:  return PKT_SIZE_1024;
        case PKT_TYPE_CMD2048:  return PKT_SIZE_2048;
        case PKT_TYPE_CMD4096:  return PKT_SIZE_4096;
        
        // Data packets
        case PKT_TYPE_DATA32:   return PKT_SIZE_32;
        case PKT_TYPE_DATA64:   return PKT_SIZE_64;
        case PKT_TYPE_DATA128:  return PKT_SIZE_128;
        case PKT_TYPE_DATA256:  return PKT_SIZE_256;
        case PKT_TYPE_DATA512:  return PKT_SIZE_512;
        case PKT_TYPE_DATA1024: return PKT_SIZE_1024;
        case PKT_TYPE_DATA2048: return PKT_SIZE_2048;
        case PKT_TYPE_DATA4096: return PKT_SIZE_4096;
        
        default:
            return 0; // Unknown packet type
    }
}

bool ProtocolHandler::IsCommandPacketType(uint8_t packet_type) {
    return (packet_type >= 0x01 && packet_type <= 0x7F);
}

bool ProtocolHandler::IsDataPacketType(uint8_t packet_type) {
    return (packet_type >= 0x80 && packet_type <= 0xFE);
}

// Get optimal packet type for given payload size
uint8_t ProtocolHandler::GetOptimalPacketType(size_t payload_size, bool is_command) {
    // Determine optimal packet size based on payload
    size_t optimal_size = 32;  // Start with minimum size
    
    if (payload_size <= 20) {
        optimal_size = 32;
    } else if (payload_size <= 52) {
        optimal_size = 64;
    } else if (payload_size <= 116) {
        optimal_size = 128;
    } else if (payload_size <= 244) {
        optimal_size = 256;
    } else if (payload_size <= 500) {
        optimal_size = 512;
    } else if (payload_size <= 1012) {
        optimal_size = 1024;
    } else if (payload_size <= 2036) {
        optimal_size = 2048;
    } else if (payload_size <= 4068) {
        optimal_size = 4096;
    } else {
        return PKT_TYPE_ERROR_VAL; // Payload too large
    }
    
    // Return appropriate packet type based on command/data flag
    if (is_command) {
        switch (optimal_size) {
            case 32:    return PKT_TYPE_CMD32;
            case 64:    return PKT_TYPE_CMD64;
            case 128:   return PKT_TYPE_CMD128;
            case 256:   return PKT_TYPE_CMD256;
            case 512:   return PKT_TYPE_CMD512;
            case 1024:  return PKT_TYPE_CMD1024;
            case 2048:  return PKT_TYPE_CMD2048;
            case 4096:  return PKT_TYPE_CMD4096;
            default:    return PKT_TYPE_ERROR_VAL;
        }
    } else {
        switch (optimal_size) {
            case 32:    return PKT_TYPE_DATA32;
            case 64:    return PKT_TYPE_DATA64;
            case 128:   return PKT_TYPE_DATA128;
            case 256:   return PKT_TYPE_DATA256;
            case 512:   return PKT_TYPE_DATA512;
            case 1024:  return PKT_TYPE_DATA1024;
            case 2048:  return PKT_TYPE_DATA2048;
            case 4096:  return PKT_TYPE_DATA4096;
            default:    return PKT_TYPE_ERROR_VAL;
        }
    }
}

// CRC calculation functions
uint16_t ProtocolHandler::CalculateSpiCrc(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc = crc >> 1;
            }
        }
    }
    
    return crc;
}

// Optimized CRC calculation (CRC at end)
uint16_t ProtocolHandler::CalculatePacketCrc(const uint8_t* packet_data, size_t packet_size) {
    // Calculate CRC over entire packet except last 2 bytes (CRC field)
    return CalculateSpiCrc(packet_data, packet_size - sizeof(uint16_t));
}

// Optimized CRC validation
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

// Flexible packet system implementations for missing functions

// Generic packet creation function - DRY principle
static size_t CreateFlexiblePacket(uint8_t* buffer, size_t buffer_size, 
                                 const void* payload_data, size_t payload_size, 
                                 bool is_command_packet) {
    // Determine optimal packet type
    uint8_t packet_type = WaveX::Protocol::ProtocolHandler::GetOptimalPacketType(payload_size, is_command_packet);
    if (packet_type == PKT_TYPE_ERROR_VAL) {
        return 0; // Payload too large
    }
    
    size_t packet_size = WaveX::Protocol::ProtocolHandler::GetPacketSizeFromType(packet_type);
    if (packet_size == 0 || packet_size > buffer_size) {
        return 0;
    }
    
    // Create packet header
    buffer[0] = packet_type;  // type
    buffer[1] = 0;           // flags
    buffer[2] = 0;           // seq (low byte)
    buffer[3] = 0;           // seq (high byte)
    buffer[4] = payload_size; // len
    
    // Copy payload data
    if (payload_size > 0 && payload_data != NULL) {
        memcpy(buffer + 5, payload_data, payload_size);
    }
    
    // Zero pad remaining payload area
    WaveX::Protocol::ProtocolHandler::ZeroPadPacket(buffer + 5 + payload_size, 
                                                   packet_size - 5 - payload_size, 0);
    
    // Calculate CRC at end
    uint16_t crc = WaveX::Protocol::ProtocolHandler::CalculatePacketCrc(buffer, packet_size);
    buffer[packet_size - 2] = crc & 0xFF;
    buffer[packet_size - 1] = (crc >> 8) & 0xFF;
    
    return packet_size;
}

// Create error packet using flexible packet system
size_t ProtocolHandler::CreateErrorPacket(uint8_t* buffer, size_t buffer_size, const ErrorMessage& err) {
    return CreateFlexiblePacket(buffer, buffer_size, &err, sizeof(ErrorMessage), true);
}

// Create sample status packet using flexible packet system
size_t ProtocolHandler::CreateSampleStatusPacket(uint8_t* buffer, size_t buffer_size, const SampleStatusMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(SampleStatusMessage), true);
}

// Create browse response packet using flexible packet system
size_t ProtocolHandler::CreateBrowseRespPacket(uint8_t* buffer, size_t buffer_size,
                                             uint32_t total_count,
                                             const FileEntryWire* entries,
                                             uint8_t n) {
    // Calculate payload size: total_count (4 bytes) + n_entries (1 byte) + entries array
    size_t payload_size = sizeof(uint32_t) + sizeof(uint8_t) + (size_t)n * sizeof(FileEntryWire);
    
    // Create temporary payload buffer
    uint8_t temp_payload[1024]; // Should be large enough for most cases
    if (payload_size > sizeof(temp_payload)) {
        return 0; // Payload too large
    }
    
    // Copy total_count first
    memcpy(temp_payload, &total_count, sizeof(uint32_t));
    
    // Copy n_entries count
    temp_payload[sizeof(uint32_t)] = n;
    
    // Copy entries array
    if (n > 0 && entries != NULL) {
        memcpy(temp_payload + sizeof(uint32_t) + sizeof(uint8_t), entries, (size_t)n * sizeof(FileEntryWire));
    }
    
    return CreateFlexiblePacket(buffer, buffer_size, temp_payload, payload_size, false);
}

// Create sample path response packet using flexible packet system
size_t ProtocolHandler::CreateSamplePathResponsePacket(uint8_t* buffer, size_t buffer_size,
                                                       const SamplePathResponseMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(SamplePathResponseMessage), true);
}

// Create control change packet using flexible packet system
size_t ProtocolHandler::CreateControlChangePacket(uint8_t* buffer, size_t buffer_size,
                                                uint8_t parameter, uint8_t channel, uint16_t value) {
    ControlChangeMessage msg;
    msg.parameter = parameter;
    msg.channel = channel;
    msg.value = value;
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(ControlChangeMessage), true);
}

// Create note on packet using flexible packet system
size_t ProtocolHandler::CreateNoteOnPacket(uint8_t* buffer, size_t buffer_size,
                                         uint8_t note, uint8_t velocity, uint8_t channel) {
    NoteMessage msg;
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = channel;
    msg.reserved = 0;
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(NoteMessage), true);
}

// Create note off packet using flexible packet system
size_t ProtocolHandler::CreateNoteOffPacket(uint8_t* buffer, size_t buffer_size,
                                          uint8_t note, uint8_t channel) {
    NoteMessage msg;
    msg.note = note;
    msg.velocity = 0;
    msg.channel = channel;
    msg.reserved = 0;
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(NoteMessage), true);
}

// Create sample control packet using flexible packet system
size_t ProtocolHandler::CreateSampleCtrlPacket(uint8_t* buffer, size_t buffer_size,
                                             const SampleCtrlMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(SampleCtrlMessage), true);
}

// Create preview request packet using flexible packet system
size_t ProtocolHandler::CreatePreviewReqPacket(uint8_t* buffer, size_t buffer_size,
                                             const PreviewReqMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(PreviewReqMessage), true);
}

// Create data request packet using flexible packet system
size_t ProtocolHandler::CreateDataRequestPacket(uint8_t* buffer, size_t buffer_size,
                                              const DataRequestMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(DataRequestMessage), true);
}

// Create meter push packet using flexible packet system
size_t ProtocolHandler::CreateMeterPushPacket(uint8_t* buffer, size_t buffer_size,
                                            const MeterPushMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(MeterPushMessage), true);
}

// Create heartbeat packet using flexible packet system
size_t ProtocolHandler::CreateHeartbeatPacket(uint8_t* buffer, size_t buffer_size,
                                            const HeartbeatMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(HeartbeatMessage), true);
}

// Create ACK packet using flexible packet system
size_t ProtocolHandler::CreateAckPacket(uint8_t* buffer, size_t buffer_size,
                                      const AckMessage& ack) {
    return CreateFlexiblePacket(buffer, buffer_size, &ack, sizeof(AckMessage), true);
}

// Create sample play index packet using flexible packet system
size_t ProtocolHandler::CreateSamplePlayIndexPacket(uint8_t* buffer, size_t buffer_size,
                                                  const SamplePlayIndexMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(SamplePlayIndexMessage), true);
}

// Create sample get path packet using flexible packet system
size_t ProtocolHandler::CreateSampleGetPathPacket(uint8_t* buffer, size_t buffer_size,
                                                const SampleGetPathMessage& msg) {
    return CreateFlexiblePacket(buffer, buffer_size, &msg, sizeof(SampleGetPathMessage), true);
}

// Parse browse request from flexible packet
bool ProtocolHandler::ParseBrowseReq(const uint8_t* buffer, char* path_out, size_t path_max,
                                     uint32_t& start_index, uint8_t& max_entries) {
    if (buffer == NULL || path_out == NULL) {
        return false;
    }
    
    // Parse browse request payload: path (null-terminated) + start_index (4 bytes) + max_entries (1 byte)
    size_t path_len = strlen((const char*)buffer);
    if (path_len >= path_max) {
        return false; // Path too long
    }
    
    // Extract path
    strncpy(path_out, (const char*)buffer, path_max - 1);
    path_out[path_max - 1] = '\0';
    
    // Extract start_index and max_entries
    const uint8_t* data_ptr = buffer + path_len + 1;
    if (data_ptr + sizeof(uint32_t) + sizeof(uint8_t) > buffer + 1024) { // Safety check
        return false;
    }
    
    start_index = *(const uint32_t*)data_ptr;
    data_ptr += sizeof(uint32_t);
    max_entries = *data_ptr;
    
    return true;
}

// Parse sample play request from flexible packet
bool ProtocolHandler::ParseSamplePlayReq(const uint8_t* buffer, char* path_out, size_t path_max) {
    if (buffer == NULL || path_out == NULL) {
        return false;
    }
    
    // Parse sample play request payload: path (null-terminated)
    strncpy(path_out, (const char*)buffer, path_max - 1);
    path_out[path_max - 1] = '\0';
    return true;
}

} // namespace Protocol
} // namespace WaveX