#include "test_helpers.h"

#include "../mocks/esp32_mocks.h"

#include <cstring>
#include <map>
#include <queue>

namespace WaveX {
namespace Test {

std::vector<uint8_t> PacketGenerator::CreateTestPacket(uint8_t msg_type,
                                                       const void* payload,
                                                       size_t payload_size) {
    std::vector<uint8_t> packet;
    packet.reserve(8 + payload_size);

    packet.push_back(msg_type);
    packet.push_back(static_cast<uint8_t>(payload_size & 0xFF));
    packet.push_back(static_cast<uint8_t>((payload_size >> 8) & 0xFF));

    if (payload && payload_size > 0) {
        const uint8_t* payload_bytes = static_cast<const uint8_t*>(payload);
        packet.insert(packet.end(), payload_bytes, payload_bytes + payload_size);
    }

    return packet;
}

std::vector<uint8_t> PacketGenerator::CreateControlChangePacket(uint8_t parameter,
                                                                uint8_t channel,
                                                                uint16_t value) {
    struct {
        uint8_t parameter;
        uint8_t channel;
        uint16_t value;
    } payload = {parameter, channel, value};

    return CreateTestPacket(0x01, &payload, sizeof(payload));
}

std::vector<uint8_t> PacketGenerator::CreateHeartbeatPacket(uint32_t uptime_ms,
                                                            uint32_t rx_total,
                                                            uint32_t loop_counter) {
    struct {
        uint32_t uptime_ms;
        uint32_t rx_total;
        uint32_t loop_counter;
        uint16_t cpu_avg_percent;
        uint16_t cpu_min_percent;
        uint16_t cpu_max_percent;
    } payload = {uptime_ms, rx_total, loop_counter, 0, 0, 0};

    return CreateTestPacket(0x12, &payload, sizeof(payload));
}

std::vector<uint8_t> PacketGenerator::CreateBrowseReqPacket(const std::string& path,
                                                            uint32_t start_index,
                                                            uint8_t max_entries) {
    std::vector<uint8_t> payload;
    payload.reserve(4 + 1 + path.length());

    payload.push_back(static_cast<uint8_t>(start_index & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((start_index >> 24) & 0xFF));
    payload.push_back(max_entries);
    payload.insert(payload.end(), path.begin(), path.end());

    return CreateTestPacket(0x30, payload.data(), payload.size());
}

void UARTInjector::InjectUARTData(uart_port_t port, const uint8_t* data, size_t length) {
    extern std::map<uart_port_t, std::queue<uint8_t>> g_uart_rx_buffers;
    auto& rx_buffer = g_uart_rx_buffers[port];

    for (size_t i = 0; i < length; ++i) {
        rx_buffer.push(data[i]);
    }
}

void UARTInjector::ClearUARTBuffer(uart_port_t port) {
    extern std::map<uart_port_t, std::queue<uint8_t>> g_uart_rx_buffers;
    extern std::map<uart_port_t, std::vector<uint8_t>> g_uart_tx_buffers;

    g_uart_rx_buffers[port] = std::queue<uint8_t>();
    g_uart_tx_buffers[port].clear();
}

std::vector<uint8_t> UARTInjector::GetUARTTransmittedData(uart_port_t port) {
    extern std::map<uart_port_t, std::vector<uint8_t>> g_uart_tx_buffers;
    return g_uart_tx_buffers[port];
}

}  // namespace Test
}  // namespace WaveX
