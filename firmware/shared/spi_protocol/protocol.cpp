#include "protocol.h"
#include <string.h>

namespace WaveX {
namespace Protocol {

size_t ProtocolHandler::CreateControlChangePacket(uint8_t* buffer, size_t buffer_size,
                                                uint8_t parameter, uint8_t channel, uint16_t value)
{
    if (buffer_size < sizeof(PacketHeader) + sizeof(ControlChangeMessage)) {
        return 0;
    }
    
    Packet* packet = reinterpret_cast<Packet*>(buffer);
    
    // Set header
    packet->header.sync = SYNC_BYTE;
    packet->header.type = MSG_CONTROL_CHANGE;
    packet->header.length = sizeof(ControlChangeMessage);
    
    // Set payload
    ControlChangeMessage* msg = reinterpret_cast<ControlChangeMessage*>(packet->payload);
    msg->parameter = parameter;
    msg->channel = channel;
    msg->value = value;
    
    // Calculate checksum
    packet->header.checksum = CalculateChecksum(packet->payload, packet->header.length);
    
    return sizeof(PacketHeader) + packet->header.length;
}

size_t ProtocolHandler::CreateNoteOnPacket(uint8_t* buffer, size_t buffer_size,
                                         uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (buffer_size < sizeof(PacketHeader) + sizeof(NoteMessage)) {
        return 0;
    }
    
    Packet* packet = reinterpret_cast<Packet*>(buffer);
    
    // Set header
    packet->header.sync = SYNC_BYTE;
    packet->header.type = MSG_NOTE_ON;
    packet->header.length = sizeof(NoteMessage);
    
    // Set payload
    NoteMessage* msg = reinterpret_cast<NoteMessage*>(packet->payload);
    msg->note = note;
    msg->velocity = velocity;
    msg->channel = channel;
    msg->reserved = 0;
    
    // Calculate checksum
    packet->header.checksum = CalculateChecksum(packet->payload, packet->header.length);
    
    return sizeof(PacketHeader) + packet->header.length;
}

size_t ProtocolHandler::CreateNoteOffPacket(uint8_t* buffer, size_t buffer_size,
                                          uint8_t note, uint8_t channel)
{
    if (buffer_size < sizeof(PacketHeader) + sizeof(NoteMessage)) {
        return 0;
    }
    
    Packet* packet = reinterpret_cast<Packet*>(buffer);
    
    // Set header
    packet->header.sync = SYNC_BYTE;
    packet->header.type = MSG_NOTE_OFF;
    packet->header.length = sizeof(NoteMessage);
    
    // Set payload
    NoteMessage* msg = reinterpret_cast<NoteMessage*>(packet->payload);
    msg->note = note;
    msg->velocity = 0;
    msg->channel = channel;
    msg->reserved = 0;
    
    // Calculate checksum
    packet->header.checksum = CalculateChecksum(packet->payload, packet->header.length);
    
    return sizeof(PacketHeader) + packet->header.length;
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
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    
    if (packet->header.type != MSG_CONTROL_CHANGE || 
        packet->header.length != sizeof(ControlChangeMessage)) {
        return false;
    }
    
    memcpy(&msg, packet->payload, sizeof(ControlChangeMessage));
    return true;
}

bool ProtocolHandler::ParseNoteMessage(const uint8_t* buffer, NoteMessage& msg)
{
    const Packet* packet = reinterpret_cast<const Packet*>(buffer);
    
    if ((packet->header.type != MSG_NOTE_ON && packet->header.type != MSG_NOTE_OFF) ||
        packet->header.length != sizeof(NoteMessage)) {
        return false;
    }
    
    memcpy(&msg, packet->payload, sizeof(NoteMessage));
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