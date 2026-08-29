#pragma once

#include "../../../shared/config/logging_config.h"
#include "daisy_seed.h"

namespace WaveX {
namespace Comm {

// Global hardware instance pointer (initialized by UartLinkInit or Spi_Init)
extern daisy::DaisySeed* s_hw;

void UartLinkInit(daisy::DaisySeed* hw);
void UartLinkStart();
int UartLinkSend(uint16_t msg_type, const void* payload, uint16_t len);
void UartLinkProcess();
// TX-only pump: transmits at most one queued frame. Unlike UartLinkProcess()
// this never touches RX, so it is safe to call from message handlers (which
// already run inside UartLinkProcess()'s RX dispatch). For senders that
// queue multiple frames back-to-back against the 4-deep TX queue.
void UartLinkPumpTx();
void UartLinkLogStats();

#if WAVEX_DAISY_UART_PERF_DEBUG
// Per-interval cost and throughput of the inter-MCU link. Reading RESETS the
// accumulators, so each sample describes its own interval. total_us is the
// figure that matters: how much main-loop time the link consumed while the
// audio ring needed refilling.
struct UartPerfSample {
    uint32_t calls;
    uint32_t total_us;
    uint32_t avg_us;
    uint32_t max_us;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t errors;           // crc + sync + overflow + tx, this interval
    uint32_t seq_drops;        // since boot
    uint32_t queue_overflows;  // since boot
};
void TakeUartPerf(UartPerfSample& out);
#endif

}  // namespace Comm
}  // namespace WaveX
