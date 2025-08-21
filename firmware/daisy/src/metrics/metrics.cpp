#include "metrics.h"

namespace WaveX {
namespace Metrics {

// UART RX message counter (used by message router)
volatile uint32_t g_uart_rx_msgs = 0;

uint32_t GetUartRxMessageCount() {
    return g_uart_rx_msgs;
}

void IncrementUartRxMessageCount() {
    g_uart_rx_msgs++;
}

} // namespace Metrics
} // namespace WaveX
