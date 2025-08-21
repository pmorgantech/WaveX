#pragma once

#include "daisy_seed.h"
#include <cstdint>

namespace WaveX {
namespace Storage {

// Minimal SPI SD block I/O used by FatFs diskio glue.
// Intentional thin abstraction so we can later swap to an SDIO/MMC backend
// while keeping the same FatFs-facing API surface.
namespace SdSpi {

// Initialize SPI and the SD card in SPI mode.
// - Starts at ~400 kHz, switches to ~20-24 MHz after ACMD41/CMD58.
// - CS is handled as a GPIO.
// Returns true on success.
bool Init(daisy::DaisySeed& hw,
          daisy::Pin sck,
          daisy::Pin miso,
          daisy::Pin mosi,
          daisy::Pin cs_pin,
          daisy::SpiHandle::Config::Peripheral periph = daisy::SpiHandle::Config::Peripheral::SPI_3);

// Read a single 512-byte sector into buf. Returns true on success.
bool ReadSector(uint32_t lba, uint8_t* buf);

// Write a single 512-byte sector from buf. Returns true on success.
bool WriteSector(uint32_t lba, const uint8_t* buf);

} // namespace SdSpi

} // namespace Storage
} // namespace WaveX


