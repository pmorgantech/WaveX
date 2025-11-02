#pragma once

#include "daisy_seed.h"

namespace WaveX {
namespace Comm {

// Global hardware instance pointer (initialized by UartLinkInit or Spi_Init)
extern daisy::DaisySeed* s_hw;

void UartLinkInit(daisy::DaisySeed* hw);
void UartLinkStart();
int  UartLinkSend(uint16_t msg_type, const void* payload, uint16_t len);
void UartLinkProcess();
void UartLinkLogStats();

} // namespace Comm
} // namespace WaveX


