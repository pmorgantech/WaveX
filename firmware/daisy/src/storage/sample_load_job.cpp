#include "sample_load_job.hpp"

#include "sdram_layout.h"

#include "fatfs_wav_reader.hpp"
#include "sample_file_job.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace WaveX::Storage {
using namespace AudioEngine;
using namespace Protocol;

bool SampleLoadJob::Begin(const SampleLoadMessage& request) {
    if (Busy() || reply_pending_)
        return false;
    request_ = request;
    status_ = SampleStatusMessage{};
    status_.sample_id = request.sample_id;  // request identity until admission
    copied_ = 0;
    saved_ = SampleFile::Document{};
    resident_ = ResidentSampleInfo{};
    if (!request.path[0] || !std::memchr(request.path, 0, sizeof(request.path))) {
        status_.state = SAMPLE_STATUS_LOAD_FAILED;
        status_.frames_played = SAMPLE_LOAD_FAIL_FORMAT;
        reply_pending_ = true;
        return true;
    }
    phase_ = Phase::Open;
    return true;
}

void SampleLoadJob::Fail(SampleMemMgr& memory, SampleLoadFailReason reason) {
    if (open_) {
        f_close(&file_);
        open_ = false;
    }
    if (handle_.len)
        memory.release(&handle_);
    destination_ = nullptr;
    status_.state = SAMPLE_STATUS_LOAD_FAILED;
    status_.frames_played = reason;
    reply_pending_ = true;
    phase_ = Phase::Idle;
}
void SampleLoadJob::Cancel(SampleMemMgr& memory) {
    if (Busy())
        Fail(memory, SAMPLE_LOAD_FAIL_READ);
}
void SampleLoadJob::Complete(SamplePool& pool, SamplePool::Record& record) {
    pool.SetPinned(record.sample_id, true);
    pool.NoteNewest(record.sample_id);
    status_.sample_id = record.sample_id;
    status_.state = SAMPLE_STATUS_LOAD_COMPLETE;
    status_.channels = record.payload.channels;
    status_.sample_rate = record.payload.sample_rate;
    status_.frames_played = record.payload.meta.total_frames;
    phase_ = Phase::Idle;
    reply_pending_ = true;
}

void SampleLoadJob::Pump(SamplePool& pool,
                         SampleMemMgr& memory,
                         uint8_t* aligned_io,
                         uint32_t io_bytes) {
    switch (phase_) {
        case Phase::Idle:
            return;
        case Phase::Open: {
            if (auto* hit = pool.FindByPath(request_.path)) {
                Complete(pool, *hit);
                return;
            }
            if (pool.Count() == SamplePool::kCapacity) {
                Fail(memory, SAMPLE_LOAD_FAIL_REGISTRY_FULL);
                return;
            }
            auto result = f_open(&file_, request_.path, FA_READ);
            if (result != FR_OK && std::strncmp(request_.path, "0:", 2) != 0) {
                char alternate[sizeof(request_.path) + 2];
                std::snprintf(alternate, sizeof(alternate), "0:%s", request_.path);
                result = f_open(&file_, alternate, FA_READ);
            }
            if (result != FR_OK) {
                Fail(memory, SAMPLE_LOAD_FAIL_OPEN);
                return;
            }
            open_ = true;
            phase_ = Phase::Header;
            return;
        }
        case Phase::Header: {
            Wav::WavInfo wav;
            FatFsWavReader reader(file_);
            if (Wav::ParseWavHeader(reader, wav) != Wav::ParseResult::Ok ||
                !BuildResidentSampleInfo(
                    request_, wav, f_size(&file_), SdramLayout::kLargeSamplePoolBytes, resident_)) {
                Fail(memory, SAMPLE_LOAD_FAIL_FORMAT);
                return;
            }
            saved_.file_bytes = f_size(&file_);
            saved_.data_offset = wav.data_offset;
            saved_.sample.sample_rate = resident_.sample_rate;
            saved_.sample.total_frames = resident_.total_frames;
            saved_.sample.channels = resident_.channels;
            saved_.sample.bits_per_sample = resident_.bit_depth;
            saved_.sample.Resolve();
            phase_ = Phase::Sidecar;
            return;
        }
        case Phase::Sidecar: {
            const auto result = ReadSampleSidecar(request_.path, saved_, saved_.sample);
            if (result == SampleSidecarResult::Invalid || result == SampleSidecarResult::IoError) {
                Fail(memory, SAMPLE_LOAD_FAIL_FORMAT);
                return;
            }
            phase_ = Phase::Allocate;
            return;
        }
        case Phase::Allocate: {
            void* destination = nullptr;
            if (!memory.alloc(resident_.data_size, &handle_) ||
                !memory.ptr(handle_, &destination)) {
                Fail(memory, SAMPLE_LOAD_FAIL_RAM);
                return;
            }
            destination_ = static_cast<uint8_t*>(destination);
            if (f_lseek(&file_, saved_.data_offset) != FR_OK ||
                f_tell(&file_) != saved_.data_offset) {
                Fail(memory, SAMPLE_LOAD_FAIL_READ);
                return;
            }
            status_.state = SAMPLE_STATUS_LOAD_PROGRESS;
            status_.channels = resident_.channels;
            status_.sample_rate = resident_.sample_rate;
            reply_pending_ = true;
            phase_ = Phase::Read;
            return;
        }
        case Phase::Read: {
            // One bounded payload read per foreground pass. The caller lends
            // aligned AXI SRAM scratch; no pointer to it survives this Pump.
            const uint32_t budget = std::min(kReadBytes, io_bytes & ~31u);
            if (!aligned_io || (reinterpret_cast<uintptr_t>(aligned_io) & 31u) || !budget) {
                Fail(memory, SAMPLE_LOAD_FAIL_READ);
                return;
            }
            const UINT bytes = std::min(budget, resident_.data_size - copied_);
            UINT read = 0;
            if (f_read(&file_, aligned_io, bytes, &read) != FR_OK || read != bytes) {
                Fail(memory, SAMPLE_LOAD_FAIL_READ);
                return;
            }
            std::memcpy(destination_ + copied_, aligned_io, read);
            copied_ += read;
            const auto progress =
                static_cast<uint32_t>(uint64_t(copied_) * 100 / resident_.data_size);
            if (progress != status_.frames_played) {
                status_.frames_played = progress;
                reply_pending_ = true;
            }
            if (copied_ == resident_.data_size)
                phase_ = Phase::Commit;
            return;
        }
        case Phase::Commit: {
            const auto closed = f_close(&file_);
            open_ = false;
            if (closed != FR_OK) {
                Fail(memory, SAMPLE_LOAD_FAIL_READ);
                return;
            }
            SamplePool::Record* record = nullptr;
            if (pool.AdmitPath(request_.path, &record) != SamplePool::Admit::Ok) {
                Fail(memory, SAMPLE_LOAD_FAIL_REGISTRY_FULL);
                return;
            }
            FillLoadedSample(record->payload, record->sample_id, request_.path, resident_, handle_);
            SampleFile::Apply(saved_, record->payload.meta);
            handle_ = {};  // ownership moved into the fully populated Pool entry
            destination_ = nullptr;
            Complete(pool, *record);
            return;
        }
    }
}
}  // namespace WaveX::Storage
