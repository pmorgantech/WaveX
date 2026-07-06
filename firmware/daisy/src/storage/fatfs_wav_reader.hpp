#pragma once

// FatFS adapter for the shared WAV header parser (wav/wav_header_parser.hpp).
// Small header reads (8-16 B) are partial-sector reads, which FatFS serves
// out of the FIL's internal sector buffer via CPU memcpy - no DMA touches
// the caller's destination, so stack destinations are fine here (same
// pattern the old OpenWav used).

#include "ff.h"

#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Storage {

class FatFsWavReader {
   public:
    explicit FatFsWavReader(FIL& file) : file_(file) {}

    size_t Read(void* dst, size_t n) {
        UINT bytes_read = 0;
        if (f_read(&file_, dst, static_cast<UINT>(n), &bytes_read) != FR_OK) {
            return 0;
        }
        return bytes_read;
    }
    bool Seek(uint32_t abs_offset) { return f_lseek(&file_, abs_offset) == FR_OK; }
    uint32_t Tell() const { return static_cast<uint32_t>(f_tell((&file_))); }

   private:
    FIL& file_;
};

}  // namespace Storage
}  // namespace WaveX
