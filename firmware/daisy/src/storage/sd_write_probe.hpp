#pragma once
#include "ff.h"

#include <cstdint>

namespace WaveX::Storage {
// Debug-console-only foreground job. Files are exclusively created with 8.3
// scratch names; successful readback removes only the file this job created.
// Failed files remain for inspection. No raw-sector writes or formatting.
class SdWriteProbe {
   public:
    enum class Phase : uint8_t {
        Idle,
        Create,
        Write,
        Sync,
        CloseWrite,
        OpenRead,
        Read,
        CloseRead,
        Remove,
        Done,
        Failed
    };
    // Pattern: 0 offset hash, 1 zero, 2 0xff, 3 alternating 0xaa/0x55.
    bool Begin(uint32_t bytes,
               uint32_t chunk,
               uint32_t shift,
               uint32_t prefix,
               uint32_t pattern = 0,
               uint32_t directory = 0,
               uint32_t gap_ms = 0);
    void Pump();
    bool Busy() const { return phase_ >= Phase::Create && phase_ <= Phase::Remove; }
    void Status(char* reply, size_t size, int32_t sequence) const;

   private:
    void Fail(uint32_t result);
    FIL file_{};  // AXI SRAM, including FatFs's DMA-visible sector buffer.
    alignas(32) uint8_t buffer_[4096 + 32]{};
    char path_[40]{};
    uint32_t bytes_ = 0, chunk_ = 0, shift_ = 0, prefix_ = 0, offset_ = 0;
    uint32_t pattern_ = 0, directory_ = 0, gap_ms_ = 0, next_ms_ = 0;
    uint32_t generation_ = 0, started_ = 0, elapsed_ = 0, result_ = 0;
    Phase phase_ = Phase::Idle, failed_phase_ = Phase::Idle;
    bool open_ = false;
};
}  // namespace WaveX::Storage
