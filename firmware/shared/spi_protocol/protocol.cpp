#include "protocol.h"
#include <string.h>

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

} // namespace Protocol
} // namespace WaveX 