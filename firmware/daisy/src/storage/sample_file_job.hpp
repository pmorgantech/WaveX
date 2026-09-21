#pragma once

#include "ff.h"

#include "wxcf/sample_file.hpp"

namespace WaveX::Storage {
enum class SampleSidecarResult { Missing, Loaded, Invalid, IoError };
// Foreground only. Uses one static AXI SRAM FIL; never reentrant or callback-safe.
bool ProbeSampleFile(const char* path, SampleFile::Document& geometry);
SampleSidecarResult ReadSampleSidecar(const char* path,
                                      const SampleFile::Document& geometry,
                                      Protocol::SampleMetadata& metadata);

class SampleFileJob {
   public:
    bool Begin(const Protocol::SampleFileOpMessage& request,
               const char* source,
               const Protocol::SampleMetadata& metadata);
    void Pump();
    void Cancel();
    bool Busy() const { return phase_ != Phase::Idle; }
    uint8_t Error() const { return error_; }
    bool CardIoFailed() const { return card_io_failed_; }
    uint8_t Progress() const;
    const char* Destination() const { return destination_; }
    SampleFileJob() = default;
    SampleFileJob(const SampleFileJob&) = delete;
    SampleFileJob& operator=(const SampleFileJob&) = delete;

   private:
    enum class Phase { Idle, Probe, Prepare, Copy, WriteSidecar, Publish };
    void Finish(uint8_t error);
    static bool Write(void*, const void*, size_t);
    FIL input_{}, output_{};  // AXI SRAM, never place this object on the device stack
    alignas(32) uint8_t block_[4096]{};
    SampleFile::Document snapshot_;
    char source_[Protocol::BROWSE_PATH_MAX]{}, destination_[Protocol::BROWSE_PATH_MAX]{};
    char sidecar_[Protocol::BROWSE_PATH_MAX + 8]{}, backup_[Protocol::BROWSE_PATH_MAX + 12]{};
    char temporary_[Protocol::BROWSE_PATH_MAX + 24]{}, wave_temp_[Protocol::BROWSE_PATH_MAX + 24]{};
    uint32_t request_id_ = 0, copied_ = 0;
    bool copy_ = false, input_open_ = false, output_open_ = false;
    bool temp_owned_ = false, wave_owned_ = false, sidecar_owned_ = false;
    bool card_io_failed_ = false;
    uint8_t error_ = Protocol::SAMPLE_FILE_OK;
    Phase phase_ = Phase::Idle;
};
}  // namespace WaveX::Storage
