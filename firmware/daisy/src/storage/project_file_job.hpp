#pragma once

#include "ff.h"

#include "wxcf/project_file.hpp"
#include <optional>

namespace WaveX::Storage {
// Foreground-only file transaction. The owner supplies private, immutable save
// input or private load scratch and keeps it alive until Busy() becomes false.
// Loading NEVER installs runtime state. Place this FIL-bearing object in AXI
// SRAM, not the DTCM stack; codec adapters transfer less than one sector.
class ProjectFileJob {
   public:
    enum class Result {
        Idle,
        Working,
        Saved,
        Loaded,
        Cancelled,
        BadName,
        Exists,
        NoSpace,
        NotFound,
        Invalid,
        IoError
    };
    bool SaveCopy(const Sequencer::Project& project, uint32_t request_id);
    bool Load(const char* name, Sequencer::Project& scratch);
    void Pump();
    void Cancel();
    bool Busy() const { return result_ == Result::Working; }
    Result Status() const { return result_; }
    uint32_t FileBytes() const { return bytes_; }

    ProjectFileJob() = default;
    ProjectFileJob(const ProjectFileJob&) = delete;
    ProjectFileJob& operator=(const ProjectFileJob&) = delete;

   private:
    enum class Phase { Validate, OpenSave, Write, Publish, OpenLoad, Read };
    bool Begin(const char* name);
    void Finish(Result result);
    static bool Count(void* context, const void*, size_t bytes);
    static bool Read(void* context, void* data, size_t bytes);
    static bool Write(void* context, const void* data, size_t bytes);
    static bool Eof(void* context);
    FIL file_{};
    const Sequencer::Project* source_ = nullptr;
    Sequencer::Project* scratch_ = nullptr;
    std::optional<ProjectFile::Encoder> encoder_;
    std::optional<ProjectFile::Decoder> decoder_;
    char destination_[96]{}, temporary_[112]{};
    uint32_t bytes_ = 0;
    bool open_ = false, owned_temp_ = false, io_failed_ = false;
    Phase phase_ = Phase::Validate;
    Result result_ = Result::Idle;
};
}  // namespace WaveX::Storage
