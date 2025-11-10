#ifndef SHARED_TEST_HELPERS_H
#define SHARED_TEST_HELPERS_H

#include "protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace WaveX {
namespace Test {

// Helper to create protocol test packets
class ProtocolTestHelper {
   public:
    // Create a valid WaveX packet for testing
    static std::vector<uint8_t> CreateWaveXPacket(uint8_t msg_type,
                                                  const void* payload,
                                                  size_t payload_size,
                                                  uint16_t sequence = 0,
                                                  uint8_t flags = 0);

    // Create packets for specific message types
    static std::vector<uint8_t> CreateControlChangePacket(uint8_t parameter,
                                                          uint8_t channel,
                                                          uint16_t value);
    static std::vector<uint8_t> CreateNoteOnPacket(uint8_t note, uint8_t velocity, uint8_t channel);
    static std::vector<uint8_t> CreateHeartbeatPacket(uint32_t uptime_ms,
                                                      uint32_t rx_total,
                                                      uint32_t loop_counter);
    static std::vector<uint8_t> CreateBrowseReqPacket(const std::string& path,
                                                      uint32_t start_index,
                                                      uint8_t max_entries);
    static std::vector<uint8_t> CreateBrowseRespPacket(
        uint32_t total_count, const std::vector<Protocol::FileEntryWire>& entries);

    // Create invalid packets for error testing
    static std::vector<uint8_t> CreateInvalidCRCPacket(uint8_t msg_type,
                                                       const void* payload,
                                                       size_t payload_size);
    static std::vector<uint8_t> CreateMalformedPacket();
    static std::vector<uint8_t> CreateOversizedPacket(size_t size);

    // Validate packet structure
    static bool ValidatePacketStructure(const uint8_t* packet, size_t length);

    // Extract packet components
    static bool ExtractPacketComponents(const uint8_t* packet,
                                        size_t length,
                                        uint8_t& msg_type,
                                        uint16_t& sequence,
                                        uint8_t& flags,
                                        std::vector<uint8_t>& payload);
};

// CRC test vectors
class CRCTestVectors {
   public:
    struct TestVector {
        const uint8_t* data;
        size_t length;
        uint16_t expected_crc;
    };

    static const TestVector* GetTestVectors();
    static size_t GetTestVectorCount();
};

}  // namespace Test
}  // namespace WaveX

#endif  // SHARED_TEST_HELPERS_H
