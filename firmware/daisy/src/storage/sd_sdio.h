#pragma once

#include "../../../shared/config/hardware_config.h"

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)

#include "daisy_seed.h"

namespace WaveX {
namespace Storage {
namespace SdSdio {

// Initialize SDMMC (SDIO) and mount FATFS on "/".
// If auto_format is true and no filesystem is found, a FAT filesystem will be created.
// Returns true on successful mount.
//
// Starts at WAVEX_DAISY_SD_CARD_SPEED and steps down one bus clock at a time
// until the card mounts and reads, so a card or harness that cannot hold the
// fastest rate lands on the fastest rate it CAN hold instead of failing.
bool InitAndMount(daisy::DaisySeed& hw, bool auto_format);

// Drop to the next slower bus clock and remount, for use when the card is
// mounted but returning data CRC errors under load - a marginal-timing
// signature that a boot-time probe cannot detect, because it only appears
// after sustained transfer. Returns false when already at the slowest rate.
// Any open file handles are invalidated; callers must reopen.
bool DowngradeSpeed();

// Current bus clock, e.g. "STANDARD/25MHz". Never null.
const char* CurrentSpeedName();

}  // namespace SdSdio
}  // namespace Storage
}  // namespace WaveX

#endif  // WAVEX_DAISY_SD_CARD_ENABLED && WAVEX_DAISY_SD_CARD_BACKEND == 1
