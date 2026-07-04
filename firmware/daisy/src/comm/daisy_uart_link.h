#pragma once

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

}  // namespace Comm
}  // namespace WaveX
