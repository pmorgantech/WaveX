#pragma once

#include "daisy_core.h"

#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Comm {
namespace Uart4Dma {

using RxCallback = void (*)(const uint8_t* data, size_t size);

bool Init(uint32_t baudrate, daisy::Pin tx_pin, daisy::Pin rx_pin);
bool StartReceive(uint8_t* buffer, size_t size, RxCallback callback);
bool StopReceive();
bool IsReceiving();

bool StartTransmit(uint8_t* buffer, size_t size);
bool TakeTransmitResult(bool& success);
bool IsTransmitting();

uint32_t TakeError();

}  // namespace Uart4Dma
}  // namespace Comm
}  // namespace WaveX
