#include "test_helpers.h"

#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace WaveX {
namespace Test {

std::vector<uint8_t> ProtocolTestHelper::CreateWaveXPacket(
    uint8_t msg_type, const void* payload, size_t payload_size, uint16_t sequence, uint8_t flags) {
    // Determine optimal packet size. GetOptimalSizeCode takes the PAYLOAD
    // size (it accounts for header+CRC itself); CreateWaveXPacket below
    // repeats the same computation, so the vector is sized exactly.
    uint8_t size_code = Protocol::ProtocolHandler::GetOptimalSizeCode(payload_size);
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
    Protocol::ControlChangeMessage msg(parameter, channel, value);
    return CreateWaveXPacket(Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
}

std::vector<uint8_t> ProtocolTestHelper::CreateNoteOnPacket(uint8_t note,
                                                            uint8_t velocity,
                                                            uint8_t channel) {
    Protocol::NoteMessage msg(note, velocity, channel);
    return CreateWaveXPacket(Protocol::MSG_NOTE_ON, &msg, sizeof(msg));
}

std::vector<uint8_t> ProtocolTestHelper::CreateHeartbeatPacket(uint32_t uptime_ms,
                                                               uint32_t rx_total,
                                                               uint32_t loop_counter) {
    Protocol::HeartbeatMessage msg(uptime_ms, rx_total, loop_counter);
    return CreateWaveXPacket(Protocol::MSG_HEARTBEAT, &msg, sizeof(msg));
}

// (CreateBrowseReqPacket was deleted: it built the dead u32-start-index
// browse-request wire format that ParseBrowseReq used to parse, which was
// removed from protocol.cpp in review H6/M10. The live format is
// [start_index u8][path][NUL]; a helper emitting the dead layout was a trap
// for the next test author.)

std::vector<uint8_t> ProtocolTestHelper::CreateBrowseRespPacket(
    uint32_t total_count, const std::vector<Protocol::FileEntryWire>& entries) {
    std::vector<uint8_t> payload;
    Protocol::BrowseRespHeader header(total_count, static_cast<uint8_t>(entries.size()));
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
    if (!packet || length < 6)
        return false;  // header(4) + crc(2) is the minimum wire size

    // Check if packet size matches size code
    uint8_t flags_size = packet[0];
    uint8_t size_code = flags_size & PKT_SIZE_MASK;
    size_t expected_size = Protocol::ProtocolHandler::GetPacketSizeFromCode(size_code);

    return length >= expected_size;
}

bool ProtocolTestHelper::ExtractPacketComponents(const uint8_t* packet,
                                                 size_t length,
                                                 uint8_t& msg_type,
                                                 uint16_t& sequence,
                                                 uint8_t& flags,
                                                 std::vector<uint8_t>& payload) {
    if (!packet || length < 6)
        return false;  // header(4) + crc(2)

    uint8_t flags_size = packet[0];
    msg_type = packet[1];
    sequence = static_cast<uint16_t>(packet[2] | (packet[3] << 8));
    // Flags live in the 4 MSB of flags_size and stay there on extraction.
    // (This used to be `(flags_size >> 4) & 0xF0`, which shifts the flags
    // into the low nibble and then masks the now-empty high nibble - the
    // result was always 0, so any flags assertion written against this
    // helper passed vacuously.)
    flags = PKT_GET_FLAGS(flags_size);

    uint8_t size_code = flags_size & PKT_SIZE_MASK;
    size_t packet_size = Protocol::ProtocolHandler::GetPacketSizeFromCode(size_code);

    if (length < packet_size)
        return false;

    // Header is 4 bytes ([0] flags_size, [1] msg_type, [2..3] seq); the CRC
    // takes the last 2. (The old `- 5 - 2` dropped the payload's final byte.)
    size_t payload_size = packet_size - 4 - 2;
    payload.assign(packet + 4, packet + 4 + payload_size);

    return true;
}

// CRC known-answer vectors for CRC-16/CCITT-FALSE (poly 0x1021, init
// 0xFFFF, no reflection, no final XOR), the algorithm CalculateWaveXCrc /
// CalculateUartCrc implement. "123456789" -> 0x29B1 is the published check
// value for this CRC; the others were computed independently against the
// algorithm's definition. The empty-input entry is 0x0000, NOT the 0xFFFF a
// textbook CRC would give: CalculateWaveXCrc deliberately special-cases
// null/empty input to 0, and these vectors pin that contract too.
// (The previous values for "A", "Hello, World!", the binary vector, and ""
// were wrong for this CRC variant - they never failed anything only because
// no test compiled this file.)
static const CRCTestVectors::TestVector crc_test_vectors[] = {
    {(const uint8_t*)"", 0, 0x0000},
    {(const uint8_t*)"A", 1, 0xB915},
    {(const uint8_t*)"123456789", 9, 0x29B1},
    {(const uint8_t*)"Hello, World!", 13, 0x67DA},
    {(const uint8_t*)"\x00\x01\x02\x03", 4, 0xE5F1},
};

const CRCTestVectors::TestVector* CRCTestVectors::GetTestVectors() {
    return crc_test_vectors;
}

size_t CRCTestVectors::GetTestVectorCount() {
    return sizeof(crc_test_vectors) / sizeof(crc_test_vectors[0]);
}

}  // namespace Test
}  // namespace WaveX
