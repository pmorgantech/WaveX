#pragma once
#include "ff.h"
namespace WaveX::Storage {
// Card content roots from track-and-patch-model.md and sequencer.md.
inline constexpr const char* kCardDirectories[] = {"0:/wavex",
                                                   "0:/wavex/samples",
                                                   "0:/wavex/sfz",
                                                   "0:/wavex/instruments",
                                                   "0:/wavex/banks",
                                                   "0:/wavex/projects",
                                                   "0:/wavex/recordings",
                                                   "0:/wavex/patterns"};
inline bool CreateCardDirectories() {
    for (const char* path: kCardDirectories) {
        const auto result = f_mkdir(path);
        if (result != FR_OK && result != FR_EXIST)
            return false;
    }
    return true;
}
}  // namespace WaveX::Storage
