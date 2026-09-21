#pragma once
#include "ff.h"

#include "wxcf/sample_file.hpp"

namespace WaveX::Storage {
// Foreground-only cooperative writer. PCM remains owned and immutable until
// Busy becomes false. Place this object in AXI SRAM, never on the DTCM stack.
class RecordingSave {
   public:
    bool Begin(
        uint32_t request, const char* name, const int16_t* pcm, uint32_t frames, uint8_t channels);
    void Pump();
    // Cancellation before the first Pump performs no filesystem work.
    void Cancel() {
        if (Busy())
            Finish(Protocol::REC_BAD_STATE);
    }
    bool Busy() const { return phase_ != Phase::Idle; }
    uint8_t Error() const { return error_; }
    uint8_t Progress() const {
        return bytes_ ? static_cast<uint8_t>(copied_ * 100ull / bytes_) : 0;
    }
    const char* Path() const { return path_; }
    const Protocol::SampleMetadata& Metadata() const { return document_.sample; }

   private:
    enum class Phase { Idle, Open, Header, Data, Sidecar, Publish };
    void Finish(uint8_t error);
    bool Write(const void* data, size_t size);
    static bool WriteSidecar(void* context, const void* data, size_t size);
    FIL file_{};
    alignas(32) uint8_t block_[4096]{};
    SampleFile::Document document_;
    const int16_t* pcm_ = nullptr;
    uint32_t bytes_ = 0, copied_ = 0, first_ = UINT32_MAX, last_ = 0;
    char path_[Protocol::BROWSE_PATH_MAX]{}, sidecar_[Protocol::BROWSE_PATH_MAX + 8]{};
    char temp_[40]{}, meta_temp_[40]{};
    Phase phase_ = Phase::Idle;
    uint8_t error_ = Protocol::REC_OK, last_result_ = 0;
    bool open_ = false, wave_owned_ = false, meta_owned_ = false, sidecar_owned_ = false;
};
}  // namespace WaveX::Storage
