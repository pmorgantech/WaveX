#include "sample_file_job.hpp"

#include "audio/sample_load_info.hpp"
#include "bss_static.hpp"
#include "card_space.hpp"
#include "fatfs_wav_reader.hpp"
#include <cstdio>
#include <cstring>

#ifndef UNIT_TEST
#include "comm/log_ring.h"
#include "per/sdmmc.h"
extern "C" SD_HandleTypeDef hsd1;
#endif

namespace WaveX::Storage {
using namespace Protocol;
namespace {
BssStatic<FIL> reader_storage;
bool Read(void* file, void* out, size_t bytes) {
    UINT n = 0;
    return bytes < 512 &&
           f_read(static_cast<FIL*>(file), out, static_cast<UINT>(bytes), &n) == FR_OK &&
           n == bytes;
}
bool Eof(void* file) {
    return f_eof(static_cast<FIL*>(file));
}
bool Path(const char* path) {
    if (!path || !path[0])
        return false;
    for (size_t i = 0; i < BROWSE_PATH_MAX; ++i)
        if (!path[i])
            return true;
    return false;
}
bool Name(const char* name) {
    if (!name || !name[0] || name[0] == ' ')
        return false;
    for (size_t i = 0; i < FILE_NAME_MAX; ++i) {
        const char c = name[i];
        if (!c)
            return i && name[i - 1] != ' ';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '_'))
            return false;
    }
    return false;
}
SampleSidecarResult ReadOne(const char* path,
                            const SampleFile::Document& geometry,
                            SampleFile::Document& out) {
    FIL& file = reader_storage.Get();
    const auto opened = f_open(&file, path, FA_READ);
    if (opened == FR_NO_FILE || opened == FR_NO_PATH)
        return SampleSidecarResult::Missing;
    if (opened != FR_OK)
        return SampleSidecarResult::IoError;
    const bool valid = SampleFile::Read({&file, Read, nullptr, Eof}, f_size(&file), out) &&
                       SampleFile::Matches(out, geometry);
    const bool io_error = f_error(&file) != FR_OK;
    if (f_close(&file) != FR_OK || io_error)
        return SampleSidecarResult::IoError;
    return valid ? SampleSidecarResult::Loaded : SampleSidecarResult::Invalid;
}
}  // namespace
bool ProbeSampleFile(const char* path, SampleFile::Document& geometry) {
    if (!Path(path))
        return false;
    FIL& file = reader_storage.Get();
    if (f_open(&file, path, FA_READ) != FR_OK)
        return false;
    Wav::WavInfo wav;
    FatFsWavReader reader(file);
    SampleFile::Document candidate;
    candidate.file_bytes = f_size(&file);
    const bool valid = Wav::ParseWavHeader(reader, wav) == Wav::ParseResult::Ok &&
                       AudioEngine::ValidatePcmWavPayload(wav, candidate.file_bytes, UINT32_MAX);
    if (f_close(&file) != FR_OK || !valid)
        return false;
    candidate.data_offset = wav.data_offset;
    auto& m = candidate.sample;
    m.sample_rate = wav.sample_rate;
    m.channels = static_cast<uint8_t>(wav.num_channels);
    m.bits_per_sample = static_cast<uint8_t>(wav.bits_per_sample);
    m.total_frames = wav.data_size / (wav.num_channels * (wav.bits_per_sample / 8u));
    m.Resolve();
    geometry = candidate;
    return true;
}
SampleSidecarResult ReadSampleSidecar(const char* path,
                                      const SampleFile::Document& geometry,
                                      SampleMetadata& metadata) {
    if (!Path(path))
        return SampleSidecarResult::Invalid;
    char sidecar[BROWSE_PATH_MAX + 12];
    std::snprintf(sidecar, sizeof(sidecar), "%s.wxs", path);
    SampleFile::Document candidate;
    const auto first = ReadOne(sidecar, geometry, candidate);
    if (first == SampleSidecarResult::IoError)
        return first;
    if (first == SampleSidecarResult::Loaded) {
        SampleFile::Apply(candidate, metadata);
        return first;
    }
    std::snprintf(sidecar, sizeof(sidecar), "%s.wxs.bak", path);
    const auto backup = ReadOne(sidecar, geometry, candidate);
    if (backup == SampleSidecarResult::Loaded)
        SampleFile::Apply(candidate, metadata);
    return backup == SampleSidecarResult::Missing ? first : backup;
}
bool SampleFileJob::Begin(const SampleFileOpMessage& request,
                          const char* source,
                          const SampleMetadata& metadata) {
    if (Busy())
        return false;
    error_ = SAMPLE_FILE_BAD_SAMPLE;
    card_io_failed_ = false;
    if (!IsValidSampleFileOp(request) || request.op == SAMPLE_FILE_GET || !Path(source))
        return false;
    copy_ = request.op == SAMPLE_FILE_COPY;
    if (copy_ && !Name(request.name)) {
        error_ = SAMPLE_FILE_BAD_NAME;
        return false;
    }
    std::strcpy(source_, source);
    if (copy_) {
        const char* slash = std::strrchr(source, '/');
        if (!slash) {
            error_ = SAMPLE_FILE_BAD_NAME;
            return false;
        }
        const int n = std::snprintf(destination_,
                                    sizeof(destination_),
                                    "%.*s%s.wav",
                                    static_cast<int>(slash - source + 1),
                                    source,
                                    request.name);
        if (n < 0 || size_t(n) >= sizeof(destination_)) {
            error_ = SAMPLE_FILE_BAD_NAME;
            return false;
        }
    } else
        std::strcpy(destination_, source);
    snapshot_ = {};
    snapshot_.sample = metadata;
    request_id_ = request.request_id;
    copied_ = 0;
    error_ = SAMPLE_FILE_OK;
    phase_ = Phase::Probe;
    return true;
}
bool SampleFileJob::Write(void* context, const void* data, size_t bytes) {
    auto& self = *static_cast<SampleFileJob*>(context);
    UINT n = 0;
    return bytes < 512 && f_write(&self.output_, data, static_cast<UINT>(bytes), &n) == FR_OK &&
           n == bytes;
}
void SampleFileJob::Finish(uint8_t error) {
#ifndef UNIT_TEST
    // Capture the original peripheral failure before closing/cleaning owned
    // temporary files: those operations can replace the HAL error state.
    card_io_failed_ = error == SAMPLE_FILE_IO && HAL_SD_GetError(&hsd1) != HAL_SD_ERROR_NONE;
    if (error != SAMPLE_FILE_OK)
        Log::PrintLine(
            "SAMPLE_FILE: error=%u phase=%u copied=%lu input_fr=%u output_fr=%u "
            "hal_err=0x%08lX",
            unsigned(error),
            unsigned(phase_),
            static_cast<unsigned long>(copied_),
            input_open_ ? unsigned(f_error(&input_)) : 0u,
            output_open_ ? unsigned(f_error(&output_)) : 0u,
            static_cast<unsigned long>(HAL_SD_GetError(&hsd1)));
#endif
    if (input_open_ && f_close(&input_) != FR_OK)
        error = SAMPLE_FILE_IO;
    if (output_open_ && f_close(&output_) != FR_OK)
        error = SAMPLE_FILE_IO;
    input_open_ = output_open_ = false;
    if (temp_owned_ && f_unlink(temporary_) != FR_OK)
        error = SAMPLE_FILE_IO;
    if (wave_owned_ && f_unlink(wave_temp_) != FR_OK)
        error = SAMPLE_FILE_IO;
    if (sidecar_owned_ && f_unlink(sidecar_) != FR_OK)
        error = SAMPLE_FILE_IO;
    temp_owned_ = wave_owned_ = sidecar_owned_ = false;
    error_ = error;
    phase_ = Phase::Idle;
}
void SampleFileJob::Cancel() {
    if (Busy())
        Finish(SAMPLE_FILE_IO);
}
uint8_t SampleFileJob::Progress() const {
    return copy_ && snapshot_.file_bytes
               ? static_cast<uint8_t>(uint64_t(copied_) * 95 / snapshot_.file_bytes)
               : 0;
}
void SampleFileJob::Pump() {
    switch (phase_) {
        case Phase::Idle:
            return;
        case Phase::Probe: {
            SampleFile::Document geometry;
            if (!ProbeSampleFile(source_, geometry)) {
                Finish(SAMPLE_FILE_IO);
                return;
            }
            snapshot_.file_bytes = geometry.file_bytes;
            snapshot_.data_offset = geometry.data_offset;
            if (!SampleFile::Matches(snapshot_, geometry)) {
                Finish(SAMPLE_FILE_CHANGED);
                return;
            }
            std::snprintf(sidecar_, sizeof(sidecar_), "%s.wxs", destination_);
            std::snprintf(backup_, sizeof(backup_), "%s.bak", sidecar_);
            std::snprintf(temporary_,
                          sizeof(temporary_),
                          "%s-%08lx.tmp",
                          sidecar_,
                          static_cast<unsigned long>(request_id_));
            std::snprintf(wave_temp_,
                          sizeof(wave_temp_),
                          "%s-%08lx.tmp",
                          destination_,
                          static_cast<unsigned long>(request_id_));
            phase_ = Phase::Prepare;
            return;
        }
        case Phase::Prepare: {
            const uint64_t required =
                SampleFile::kFileBytes + (copy_ ? uint64_t(snapshot_.file_bytes) : 0);
            const auto space = required <= UINT32_MAX
                                   ? CheckSaveSpace(static_cast<uint32_t>(required))
                                   : SaveSpace::Full;
            if (space != SaveSpace::Ready) {
                Finish(space == SaveSpace::Full ? SAMPLE_FILE_NO_SPACE : SAMPLE_FILE_IO);
                return;
            }
            FILINFO info{};
            if (copy_) {
                const char* paths[] = {destination_, sidecar_, backup_};
                for (const auto* path: paths) {
                    const auto found = f_stat(path, &info);
                    if (found != FR_NO_FILE) {
                        Finish(found == FR_OK ? SAMPLE_FILE_EXISTS : SAMPLE_FILE_IO);
                        return;
                    }
                }
                if (f_open(&input_, source_, FA_READ) != FR_OK) {
                    Finish(SAMPLE_FILE_IO);
                    return;
                }
                input_open_ = true;
                if (f_size(&input_) != snapshot_.file_bytes) {
                    Finish(SAMPLE_FILE_CHANGED);
                    return;
                }
                const auto opened = f_open(&output_, wave_temp_, FA_WRITE | FA_CREATE_NEW);
                if (opened != FR_OK) {
                    Finish(opened == FR_EXIST ? SAMPLE_FILE_EXISTS : SAMPLE_FILE_IO);
                    return;
                }
                output_open_ = wave_owned_ = true;
                phase_ = Phase::Copy;
            } else {
                // Restore a backup after a reset between the two publish renames.
                SampleFile::Document old;
                const auto current = ReadOne(sidecar_, snapshot_, old);
                if (current == SampleSidecarResult::Invalid ||
                    current == SampleSidecarResult::IoError) {
                    Finish(SAMPLE_FILE_CHANGED);
                    return;
                }
                const auto bak = f_stat(backup_, &info);
                if (bak != FR_OK && bak != FR_NO_FILE) {
                    Finish(SAMPLE_FILE_IO);
                    return;
                }
                if (bak == FR_OK) {
                    if (current == SampleSidecarResult::Loaded) {
                        if (f_unlink(backup_) != FR_OK) {
                            Finish(SAMPLE_FILE_IO);
                            return;
                        }
                    } else {
                        if (ReadOne(backup_, snapshot_, old) != SampleSidecarResult::Loaded ||
                            f_rename(backup_, sidecar_) != FR_OK) {
                            Finish(SAMPLE_FILE_IO);
                            return;
                        }
                    }
                }
                phase_ = Phase::WriteSidecar;
            }
            return;
        }
        case Phase::Copy: {
            const uint32_t remaining = snapshot_.file_bytes - copied_;
            const UINT bytes = remaining < sizeof(block_) ? remaining : sizeof(block_);
            UINT read = 0, written = 0;
            if (f_read(&input_, block_, bytes, &read) != FR_OK || read != bytes ||
                f_write(&output_, block_, bytes, &written) != FR_OK || written != bytes) {
                Finish(SAMPLE_FILE_IO);
                return;
            }
            copied_ += bytes;
            if (copied_ == snapshot_.file_bytes) {
                const auto input_closed = f_close(&input_);
                const auto output_closed = f_close(&output_);
                input_open_ = output_open_ = false;
                if (input_closed != FR_OK || output_closed != FR_OK) {
                    Finish(SAMPLE_FILE_IO);
                    return;
                }
                phase_ = Phase::WriteSidecar;
            }
            return;
        }
        case Phase::WriteSidecar: {
            const auto opened = f_open(&output_, temporary_, FA_WRITE | FA_CREATE_NEW);
            if (opened != FR_OK) {
                Finish(opened == FR_EXIST ? SAMPLE_FILE_EXISTS : SAMPLE_FILE_IO);
                return;
            }
            output_open_ = temp_owned_ = true;
            if (!SampleFile::Write({this, nullptr, Write, nullptr}, snapshot_)) {
                Finish(SAMPLE_FILE_IO);
                return;
            }
            const auto closed = f_close(&output_);
            output_open_ = false;
            if (closed != FR_OK) {
                Finish(SAMPLE_FILE_IO);
                return;
            }
            phase_ = Phase::Publish;
            return;
        }
        case Phase::Publish: {
            if (!copy_) {
                FILINFO info{};
                const auto found = f_stat(sidecar_, &info);
                if (found != FR_NO_FILE &&
                    (found != FR_OK || f_rename(sidecar_, backup_) != FR_OK)) {
                    Finish(SAMPLE_FILE_IO);
                    return;
                }
            }
            if (f_rename(temporary_, sidecar_) != FR_OK) {
                Finish(SAMPLE_FILE_IO);
                return;
            }
            temp_owned_ = false;
            if (copy_) {
                sidecar_owned_ = true;
                if (f_rename(wave_temp_, destination_) != FR_OK) {
                    Finish(SAMPLE_FILE_IO);
                    return;
                }
                wave_owned_ = sidecar_owned_ = false;
            }
            // Keep the old sidecar as a recovery copy until the next successful
            // save preflight validates the current one. No unlink/replace gap.
            Finish(SAMPLE_FILE_OK);
            return;
        }
    }
}
}  // namespace WaveX::Storage
