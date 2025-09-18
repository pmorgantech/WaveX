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
bool InitAndMount(daisy::DaisySeed& hw, bool auto_format);

} // namespace SdSdio
} // namespace Storage
} // namespace WaveX

#endif // WAVEX_DAISY_SD_CARD_ENABLED && WAVEX_DAISY_SD_CARD_BACKEND == 1


