#pragma once

#include "config/link_config.h"
#include "daisy_uart_link.h"
#if WAVEX_SPI_LINK_ENABLED
#include "daisy_spi_link.h"
#endif

namespace WaveX {
namespace Comm {

// Main-loop only. The selected driver owns framing, sequence and DMA state.
inline int LinkSend(uint16_t type, const void* payload, uint16_t bytes) {
#if WAVEX_SPI_LINK_ENABLED
    return Spi_Send(type, payload, bytes);
#else
    return UartLinkSend(type, payload, bytes);
#endif
}
inline bool LinkTxIdle() {
#if WAVEX_SPI_LINK_ENABLED
    return Spi_TxIdle();
#else
    return UartLinkTxIdle();
#endif
}
// May service DMA, but must never dispatch an incoming command recursively.
inline void LinkPumpTx() {
#if WAVEX_SPI_LINK_ENABLED
    Spi_PumpTx();
#else
    UartLinkPumpTx();
#endif
}
inline void LinkProcess() {
#if WAVEX_SPI_LINK_ENABLED
    ProcessQueuedSpiMessage();
#else
    UartLinkProcess();
#endif
}
inline void LinkLogStats() {
#if WAVEX_SPI_LINK_ENABLED
    Spi_DebugState();
#else
    UartLinkLogStats();
#endif
}
#if WAVEX_DAISY_UART_PERF_DEBUG
using LinkPerfSample = UartPerfSample;
inline void TakeLinkPerf(LinkPerfSample& out) {
#if WAVEX_SPI_LINK_ENABLED
    Spi_TakePerf(out);
#else
    TakeUartPerf(out);
#endif
}
#endif

}  // namespace Comm
}  // namespace WaveX
