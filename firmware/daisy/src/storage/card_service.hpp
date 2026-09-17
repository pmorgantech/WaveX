#pragma once
#include "spi_protocol/protocol.h"
namespace WaveX::Storage::CardService {
void Request(const Protocol::CardOpMessage& request);
void Pump();
bool Busy();
}  // namespace WaveX::Storage::CardService
