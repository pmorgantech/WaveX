#include "test_helpers.h"

#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace WaveX {
namespace Test {

std::vector<uint8_t> ProtocolTestHelper::CreateWaveXPacket(
    uint8_t msg_type, const void* payload, size_t payload_size, uint16_t sequence, uint8_t flags) {
    // Determine optimal packet size
    uint8_t size_code =
        Protocol::ProtocolHandler::GetOptimalSizeCode(payload_size + 5);  // +5 for header
    size_t packet_size = Protocol::ProtocolHandler::GetPacketSizeFromCode(size_code);

    std::vector<uint8_t> packet(packet_size, 0);

    // Create packet using ProtocolHandler
    size_t created_size = Protocol::ProtocolHandler::CreateWaveXPacket(
        packet.data(), packet.size(), msg_type, payload, payload_size, sequence, flags);

    if (created_size == 0) {
        return {};
    }

    packet.resize(created_size);
    return packet;
}

std::vector<uint8_t> ProtocolTestHelper::CreateControlChangePacket(uint8_t parameter,
                                                                   uint8_t channel,
                                                                   uint16_t value) {
    Protocol::ControlChangeMessage msg = {parameter, channel, value};
    return CreateWaveXPacket(Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
}

std::vector<uint8_t> ProtocolTestHelper::CreateNoteOnPacket(uint8_t note,
                                                            uint8_t velocity,
                                                            uint8_t channel) {
    Protocol::NoteMessage msg = {note, velocity, channel, 0};
    return CreateWaveXPacket(Protocol::MSG_NOTE_ON, &msg, sizeof(msg));
}

std::vector<uint8_t> ProtocolTestHelper::CreateHeartbeatPacket(uint32_t uptime_ms,
                                                               uint32_t rx_total,
                                                               uint32_t loop_counter) {
    Protocol::HeartbeatMessage msg = {uptime_ms, rx_total, loop_counter, 0, 0, 0};
    return CreateWaveXPacket(Protocol::MSG_HEARTBEAT, &msg, sizeof(msg));
}

std::vector<uint8_t> ProtocolTestHelper::CreateBrowseReqPacket(const std::string& path,
                                                               uint32_t start_index,
                                                               uint8_t max_entries) {
    std::vector<uint8_t> payload;
    payload.reserve(5 + path.length());
    payload.push_back(static_cast<uint8_t>(start_index & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 24) & 0xFF));
    payload.push_back(max_entries);
    payload.insert(payload.end(), path.begin(), path.end());

    return CreateWaveXPacket(Protocol::MSG_BROWSE_REQ, payload.data(), payload.size());
}

std::vector<uint8_t> ProtocolTestHelper::CreateBrowseRespPacket(
    uint32_t total_count, const std::vector<Protocol::FileEntryWire>& entries) {
    std::vector<uint8_t> payload;
    Protocol::BrowseRespHeader header = {total_count, static_cast<uint8_t>(entries.size())};
    payload.insert(payload.end(),
                   reinterpret_cast<const uint8_t*>(&header),
                   reinterpret_cast<const uint8_t*>(&header) + sizeof(header));

    for (const auto& entry: entries) {
        payload.insert(payload.end(),
                       reinterpret_cast<const uint8_t*>(&entry),
                       reinterpret_cast<const uint8_t*>(&entry) + sizeof(entry));
    }

    return CreateWaveXPacket(Protocol::MSG_BROWSE_RESP, payload.data(), payload.size());
}

std::vector<uint8_t> ProtocolTestHelper::CreateInvalidCRCPacket(uint8_t msg_type,
                                                                const void* payload,
                                                                size_t payload_size) {
    auto packet = CreateWaveXPacket(msg_type, payload, payload_size);
    if (!packet.empty()) {
        // Corrupt CRC (last 2 bytes)
        packet[packet.size() - 2] ^= 0xFF;
        packet[packet.size() - 1] ^= 0xFF;
    }
    return packet;
}

std::vector<uint8_t> ProtocolTestHelper::CreateMalformedPacket() {
    // Return a packet that's too small to be valid
    return {0x00, 0x01, 0x02};
}

std::vector<uint8_t> ProtocolTestHelper::CreateOversizedPacket(size_t size) {
    std::vector<uint8_t> packet(size, 0xFF);
    return packet;
}

bool ProtocolTestHelper::ValidatePacketStructure(const uint8_t* packet, size_t length) {
    if (length < 5)
        return false;  // Minimum packet size

    // Check if packet size matches size code
    uint8_t flags_size = packet[0];
    uint8_t size_code = flags_size & 0x0F;
    size_t expected_size = Protocol::ProtocolHandler::GetPacketSizeFromCode(size_code);

    return length >= expected_size;
}

bool ProtocolTestHelper::ExtractPacketComponents(const uint8_t* packet,
                                                 size_t length,
                                                 uint8_t& msg_type,
                                                 uint16_t& sequence,
                                                 uint8_t& flags,
                                                 std::vector<uint8_t>& payload) {
    if (length < 5)
        return false;

    uint8_t flags_size = packet[0];
    msg_type = packet[1];
    sequence = packet[2] | (packet[3] << 8);
    flags = (flags_size >> 4) & 0xF0;

    uint8_t size_code = flags_size & 0x0F;
    size_t packet_size = Protocol::ProtocolHandler::GetPacketSizeFromCode(size_code);

    if (length < packet_size)
        return false;

    size_t payload_size = packet_size - 5 - 2;  // -5 for header, -2 for CRC
    payload.assign(packet + 4, packet + 4 + payload_size);

    return true;
}

// CRC test vectors
static const CRCTestVectors::TestVector crc_test_vectors[] = {
    {(const uint8_t*)"", 0, 0xFFFF},
    {(const uint8_t*)"A", 1, 0x538D},
    {(const uint8_t*)"123456789", 9, 0x29B1},
    {(const uint8_t*)"Hello, World!", 13, 0xE5CC},
    {(const uint8_t*)"\x00\x01\x02\x03", 4, 0x89C3},
};

const CRCTestVectors::TestVector* CRCTestVectors::GetTestVectors() {
    return crc_test_vectors;
}

size_t CRCTestVectors::GetTestVectorCount() {
    return sizeof(crc_test_vectors) / sizeof(crc_test_vectors[0]);
}

}  // namespace Test
}  // namespace WaveX
