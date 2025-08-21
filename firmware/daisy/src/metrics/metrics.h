#pragma once

#include <cstdint>

namespace WaveX {
namespace Metrics {

// UART RX message counter (used by message router)
extern volatile uint32_t g_uart_rx_msgs;

// Get current UART RX message count
uint32_t GetUartRxMessageCount();

// Increment UART RX message count
void IncrementUartRxMessageCount();

} // namespace Metrics
} // namespace WaveX
