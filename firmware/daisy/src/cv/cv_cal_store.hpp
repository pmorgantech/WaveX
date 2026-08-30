#pragma once

// SD persistence for the CV calibration table (analog-voice-board.md §3:
// "Calibration table persists on SD"). Format (documented here, the only
// place it exists): binary file 0:/wavex_cvcal.bin =
//   char magic[4] = "WXCV"
//   uint32_t version = 1
//   CvCal[WAVEX_ANALOG_CV_GROUPS_MAX]   (8 groups, plain structs)
// The table is always full-size regardless of WAVEX_ANALOG_CV_GROUPS so a
// Stage A file survives the Stage B flag flip (architecture.md §5.3).
//
// Main-loop-only (blocking FatFS I/O); load at boot after SD mount, save
// when MSG_CV_CAL_SET arrives with persist=1.

#include "config/hardware_config.h"
#include "ff.h"

#include "cv_cal.hpp"
#include <cstring>

namespace WaveX {
namespace Cv {

static constexpr char kCvCalPath[] = "0:/wavex_cvcal.bin";
static constexpr char kCvCalMagic[4] = {'W', 'X', 'C', 'V'};
static constexpr uint32_t kCvCalVersion = 1;

// No packed attribute: GCC ignores packed on the non-POD CvCal field (its
// default member initializers make it non-POD), so every file ever written
// used the natural layout below. The asserts pin that layout - CvCal is
// seven 4-byte floats and the header is 8 bytes, so there is no padding
// and the format is identical on both counts.
struct CvCalFile {
    char magic[4];
    uint32_t version;
    CvCal groups[WAVEX_ANALOG_CV_GROUPS_MAX];
};
static_assert(sizeof(CvCal) == 7 * sizeof(float), "CvCal layout is the on-disk format");
static_assert(sizeof(CvCalFile) == 8 + WAVEX_ANALOG_CV_GROUPS_MAX * sizeof(CvCal),
              "CvCalFile layout is the on-disk format");

// Loads the table into `out` (must hold WAVEX_ANALOG_CV_GROUPS_MAX
// entries). Returns false (out untouched) if the file is missing, short,
// or has the wrong magic/version - callers keep their defaults.
inline bool LoadCvCalTable(FIL& file, CvCal* out) {
    if (f_open(&file, kCvCalPath, FA_READ) != FR_OK) {
        return false;
    }
    CvCalFile data{};
    UINT bytes_read = 0;
    const FRESULT fr = f_read(&file, &data, sizeof(data), &bytes_read);
    f_close(&file);
    if (fr != FR_OK || bytes_read != sizeof(data) ||
        std::memcmp(data.magic, kCvCalMagic, sizeof(kCvCalMagic)) != 0 ||
        data.version != kCvCalVersion) {
        return false;
    }
    for (uint8_t g = 0; g < WAVEX_ANALOG_CV_GROUPS_MAX; ++g) {
        out[g] = data.groups[g];
    }
    return true;
}

// Writes the full table. Returns false on any I/O failure.
inline bool SaveCvCalTable(FIL& file, const CvCal* table) {
    if (f_open(&file, kCvCalPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    CvCalFile data{};
    std::memcpy(data.magic, kCvCalMagic, sizeof(kCvCalMagic));
    data.version = kCvCalVersion;
    for (uint8_t g = 0; g < WAVEX_ANALOG_CV_GROUPS_MAX; ++g) {
        data.groups[g] = table[g];
    }
    UINT bytes_written = 0;
    const FRESULT fr = f_write(&file, &data, sizeof(data), &bytes_written);
    f_close(&file);
    return fr == FR_OK && bytes_written == sizeof(data);
}

}  // namespace Cv
}  // namespace WaveX
