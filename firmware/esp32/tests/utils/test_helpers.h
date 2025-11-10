#ifndef ESP32_TEST_HELPERS_H
#define ESP32_TEST_HELPERS_H

#include "../../../shared/spi_protocol/protocol.h"
#include "../mocks/esp32_mocks.h"  // For uart_port_t and other types

#include <cstdint>
#include <string>
#include <vector>

namespace WaveX {
namespace Test {

// Helper to create test packets for ESP32 tests
class PacketGenerator {
   public:
    static std::vector<uint8_t> CreateTestPacket(uint8_t msg_type,
                                                 const void* payload,
                                                 size_t payload_size);
    static std::vector<uint8_t> CreateControlChangePacket(uint8_t parameter,
                                                          uint8_t channel,
                                                          uint16_t value);
    static std::vector<uint8_t> CreateHeartbeatPacket(uint32_t uptime_ms,
                                                      uint32_t rx_total,
                                                      uint32_t loop_counter);
    static std::vector<uint8_t> CreateBrowseReqPacket(const std::string& path,
                                                      uint32_t start_index,
                                                      uint8_t max_entries);
};

// Protocol test helper (alias for compatibility with existing tests)
class ProtocolTestHelper {
   public:
    // Create heartbeat packet
    static std::vector<uint8_t> CreateHeartbeatPacket(uint32_t uptime_ms,
                                                      uint32_t rx_total,
                                                      uint32_t loop_counter) {
        Protocol::HeartbeatMessage msg = {uptime_ms, rx_total, loop_counter, 256, 128, 512};
        return CreateWaveXPacket(Protocol::MSG_HEARTBEAT, &msg, sizeof(msg));
    }

    // Create WaveX packet with optional sequence and flags
    static std::vector<uint8_t> CreateWaveXPacket(uint8_t msg_type,
                                                  const void* payload,
                                                  size_t payload_size,
                                                  uint16_t sequence = 0,
                                                  uint8_t flags = 0) {
        std::vector<uint8_t> buffer(2048);
        size_t created = Protocol::ProtocolHandler::CreateWaveXPacket(
            buffer.data(), buffer.size(), msg_type, payload, payload_size, sequence, flags);

        if (created == 0) {
            return std::vector<uint8_t>();
        }

        buffer.resize(created);
        return buffer;
    }

    // Create browse response packet
    static std::vector<uint8_t> CreateBrowseRespPacket(
        uint32_t total_count, const std::vector<Protocol::FileEntryWire>& entries) {
        std::vector<uint8_t> buffer(2048);
        size_t created =
            Protocol::ProtocolHandler::CreateBrowseRespPacket(buffer.data(),
                                                              buffer.size(),
                                                              total_count,
                                                              entries.data(),
                                                              static_cast<uint8_t>(entries.size()));

        if (created == 0) {
            return std::vector<uint8_t>();
        }

        buffer.resize(created);
        return buffer;
    }
};

// Helper to inject UART data for testing
class UARTInjector {
   public:
    static void InjectUARTData(uart_port_t port, const uint8_t* data, size_t length);
    static void ClearUARTBuffer(uart_port_t port);
    static std::vector<uint8_t> GetUARTTransmittedData(uart_port_t port);
};

}  // namespace Test
}  // namespace WaveX

#endif  // ESP32_TEST_HELPERS_H
