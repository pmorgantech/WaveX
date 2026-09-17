#pragma once

#include "ff.h"

#include <cstdint>

namespace WaveX::Storage {
enum class SaveSpace { Ready, Full, IoError };

// Foreground only, before mkdir/open/truncation. The temporary copy must fit
// alongside existing files. Reserve four clusters for parent/leaf directory
// creation and long-name/temp/rename entries; write/close checks remain required.
inline SaveSpace CheckSaveSpace(uint32_t file_bytes) {
    DWORD free_clusters = 0;
    FATFS* fs = nullptr;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || !fs || !fs->csize)
        return SaveSpace::IoError;
    constexpr uint32_t sector_bytes = 512;  // FatFs _MIN_SS == _MAX_SS on Daisy
    const uint64_t cluster_bytes = static_cast<uint64_t>(fs->csize) * sector_bytes;
    const uint64_t required = (file_bytes + cluster_bytes - 1) / cluster_bytes + 4;
    return free_clusters >= required ? SaveSpace::Ready : SaveSpace::Full;
}
}  // namespace WaveX::Storage
