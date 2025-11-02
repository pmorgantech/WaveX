#pragma once

#include "daisy_seed.h"

namespace WaveX {
namespace Comm {

void UartLinkInit(daisy::DaisySeed* hw);
void UartLinkStart();
int  UartLinkSend(uint16_t msg_type, const void* payload, uint16_t len);
void UartLinkProcess();
void UartLinkLogStats();

} // namespace Comm
} // namespace WaveX


