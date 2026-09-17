#include "project_file_job.hpp"

#include "card_space.hpp"
#include <cstdio>

namespace WaveX::Storage {
namespace {
bool Directory(const char* path) {
    const auto result = f_mkdir(path);
    return result == FR_OK || result == FR_EXIST;
}
}  // namespace
bool ProjectFileJob::Begin(const char* name) {
    if (Busy())
        return false;
    if (!name || !PatternFile::ValidName(name)) {
        result_ = Result::BadName;
        return false;
    }
    std::snprintf(destination_, sizeof(destination_), "0:/wavex/projects/%s.wxp", name);
    source_ = nullptr;
    scratch_ = nullptr;
    bytes_ = 0;
    io_failed_ = false;
    result_ = Result::Working;
    return true;
}
bool ProjectFileJob::SaveCopy(const Sequencer::Project& project, uint32_t request_id) {
    if (!request_id || !Begin(project.name))
        return false;
    source_ = &project;
    std::snprintf(temporary_,
                  sizeof(temporary_),
                  "0:/wavex/projects/.%s-%08lx.tmp",
                  project.name,
                  static_cast<unsigned long>(request_id));
    // Count with the real encoder: size and validation cannot drift from the
    // wire representation. This pass writes nothing and yields like disk I/O.
    encoder_.emplace(Wxcf::IoContext{this, nullptr, Count, nullptr}, project);
    phase_ = Phase::Validate;
    return true;
}
bool ProjectFileJob::Load(const char* name, Sequencer::Project& scratch) {
    if (!Begin(name))
        return false;
    scratch_ = &scratch;
    phase_ = Phase::OpenLoad;
    return true;
}
bool ProjectFileJob::Count(void* context, const void*, size_t bytes) {
    auto& job = *static_cast<ProjectFileJob*>(context);
    if (bytes > ProjectFile::kMaxFileBytes - job.bytes_)
        return false;
    job.bytes_ += static_cast<uint32_t>(bytes);
    return true;
}
bool ProjectFileJob::Read(void* context, void* data, size_t bytes) {
    auto& job = *static_cast<ProjectFileJob*>(context);
    UINT n = 0;
    if (bytes >= 512 || f_read(&job.file_, data, static_cast<UINT>(bytes), &n) != FR_OK) {
        job.io_failed_ = true;
        return false;
    }
    return n == bytes;  // truncated content is Invalid, a disk error is IoError
}
bool ProjectFileJob::Write(void* context, const void* data, size_t bytes) {
    auto& job = *static_cast<ProjectFileJob*>(context);
    UINT n = 0;
    return bytes < 512 && f_write(&job.file_, data, static_cast<UINT>(bytes), &n) == FR_OK &&
           n == bytes;
}
bool ProjectFileJob::Eof(void* context) {
    return f_eof(&static_cast<ProjectFileJob*>(context)->file_);
}
void ProjectFileJob::Finish(Result result) {
    encoder_.reset();
    decoder_.reset();
    if (open_) {
        if (f_close(&file_) != FR_OK)
            result = Result::IoError;
        open_ = false;
    }
    if (owned_temp_) {
        if (f_unlink(temporary_) != FR_OK)
            result = Result::IoError;
        owned_temp_ = false;
    }
    source_ = nullptr;
    scratch_ = nullptr;
    result_ = result;
}
void ProjectFileJob::Cancel() {
    if (Busy())
        Finish(Result::Cancelled);
}
void ProjectFileJob::Pump() {
    if (!Busy())
        return;
    using CodecResult = ProjectFile::Result;
    switch (phase_) {
        case Phase::Validate:
        case Phase::Write: {
            CodecResult result = CodecResult::More;
            for (unsigned i = 0; i < 8 && result == CodecResult::More; ++i)
                result = encoder_->Advance();
            if (result == CodecResult::More)
                return;
            encoder_.reset();
            if (result != CodecResult::Done) {
                Finish(phase_ == Phase::Validate || result == CodecResult::Invalid
                           ? Result::Invalid
                           : Result::IoError);
                return;
            }
            if (phase_ == Phase::Validate) {
                phase_ = Phase::OpenSave;
            } else {
                const auto closed = f_close(&file_);
                open_ = false;
                if (closed != FR_OK) {
                    Finish(Result::IoError);
                    return;
                }
                phase_ = Phase::Publish;
            }
            break;
        }
        case Phase::OpenSave: {
            const auto space = CheckSaveSpace(bytes_);
            if (space != SaveSpace::Ready) {
                Finish(space == SaveSpace::Full ? Result::NoSpace : Result::IoError);
                return;
            }
            if (!Directory("0:/wavex") || !Directory("0:/wavex/projects")) {
                Finish(Result::IoError);
                return;
            }
            FILINFO info{};
            const auto found = f_stat(destination_, &info);
            if (found != FR_NO_FILE) {
                Finish(found == FR_OK ? Result::Exists : Result::IoError);
                return;
            }
            const auto opened = f_open(&file_, temporary_, FA_WRITE | FA_CREATE_NEW);
            if (opened != FR_OK) {
                Finish(opened == FR_EXIST ? Result::Exists : Result::IoError);
                return;
            }
            open_ = owned_temp_ = true;
            encoder_.emplace(Wxcf::IoContext{this, nullptr, Write, nullptr}, *source_);
            phase_ = Phase::Write;
            break;
        }
        case Phase::Publish: {
            const auto renamed = f_rename(temporary_, destination_);
            if (renamed == FR_OK)
                owned_temp_ = false;
            Finish(renamed == FR_OK      ? Result::Saved
                   : renamed == FR_EXIST ? Result::Exists
                                         : Result::IoError);
            break;
        }
        case Phase::OpenLoad: {
            const auto opened = f_open(&file_, destination_, FA_READ);
            if (opened != FR_OK) {
                Finish(opened == FR_NO_FILE || opened == FR_NO_PATH ? Result::NotFound
                                                                    : Result::IoError);
                return;
            }
            open_ = true;
            bytes_ = f_size(&file_);
            if (bytes_ > ProjectFile::kMaxFileBytes) {
                Finish(Result::Invalid);
                return;
            }
            decoder_.emplace(Wxcf::IoContext{this, Read, nullptr, Eof}, *scratch_);
            phase_ = Phase::Read;
            break;
        }
        case Phase::Read: {
            CodecResult result = CodecResult::More;
            for (unsigned i = 0; i < 8 && result == CodecResult::More; ++i)
                result = decoder_->Advance();
            if (result != CodecResult::More)
                Finish(result == CodecResult::Done ? Result::Loaded
                       : io_failed_                ? Result::IoError
                                                   : Result::Invalid);
            break;
        }
    }
}
}  // namespace WaveX::Storage
