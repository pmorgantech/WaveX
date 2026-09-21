#include "recording_save.hpp"

#include "util/wav_format.h"
#ifndef UNIT_TEST
#include "comm/log_ring.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace WaveX::Storage {
using namespace Protocol;
bool RecordingSave::Begin(
    uint32_t request, const char* name, const int16_t* pcm, uint32_t frames, uint8_t channels) {
    if (Busy())
        return false;
    error_ = REC_BAD_NAME;
    if (!name || !name[0] || name[0] == ' ')
        return false;
    size_t n = 0;
    for (; n < FILE_NAME_MAX && name[n]; ++n) {
        const char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '_'))
            return false;
    }
    if (n == FILE_NAME_MAX || name[n - 1] == ' ')
        return false;
    error_ = REC_BAD_STATE;
    if (!request || !pcm || !frames || frames > 48000u * 120u || (channels != 1 && channels != 2))
        return false;
    const int size = std::snprintf(path_, sizeof(path_), "/wavex/recordings/%s.wav", name);
    if (size < 0 || static_cast<size_t>(size) >= sizeof(path_)) {
        error_ = REC_BAD_NAME;
        return false;
    }
    std::snprintf(sidecar_, sizeof(sidecar_), "%s.wxs", path_);
    // Keep temporary directory entries short while creating the paired files.
    // Publish retains the full user name; CREATE_NEW prevents
    // a stale transaction from being overwritten.
    std::snprintf(
        temp_, sizeof(temp_), "/wavex/recordings/%08lx.tmp", static_cast<unsigned long>(request));
    std::snprintf(meta_temp_,
                  sizeof(meta_temp_),
                  "/wavex/recordings/%08lx.met",
                  static_cast<unsigned long>(request));
    document_ = {};
    auto& m = document_.sample;
    m.sample_rate = 48000;
    m.total_frames = frames;
    m.end_frame = frames;
    m.loop_end = frames;
    m.channels = channels;
    m.bits_per_sample = 16;
    pcm_ = pcm;
    bytes_ = frames * channels * 2u;
    copied_ = 0;
    first_ = UINT32_MAX;
    last_ = 0;
    document_.file_bytes = bytes_ + 44;
    document_.data_offset = 44;
    error_ = REC_OK;
    phase_ = Phase::Open;
    return true;
}
bool RecordingSave::Write(const void* data, size_t size) {
    UINT written = 0;
    const auto result = f_write(&file_, data, static_cast<UINT>(size), &written);
    last_result_ = result;
    return result == FR_OK && written == size;
}
bool RecordingSave::WriteSidecar(void* context, const void* data, size_t size) {
    auto& self = *static_cast<RecordingSave*>(context);
    if (size > sizeof(self.block_))
        return false;
    std::memcpy(self.block_, data, size);
    return self.Write(self.block_, size);
}
void RecordingSave::Finish(uint8_t error) {
#ifndef UNIT_TEST
    if (error != REC_OK)
        WaveX::Log::PrintLine("REC_SAVE error=%u phase=%u fatfs=%u bytes=%lu",
                              error,
                              unsigned(phase_),
                              last_result_,
                              static_cast<unsigned long>(copied_));
#endif
    if (open_ && f_close(&file_) != FR_OK)
        error = REC_IO;
    open_ = false;
    if (error != REC_OK) {
        if (wave_owned_)
            f_unlink(temp_);
        if (meta_owned_)
            f_unlink(meta_temp_);
        if (sidecar_owned_)
            f_unlink(sidecar_);
    }
    wave_owned_ = meta_owned_ = sidecar_owned_ = false;
    error_ = error;
    phase_ = Phase::Idle;
    pcm_ = nullptr;
}
void RecordingSave::Pump() {
    switch (phase_) {
        case Phase::Idle:
            return;
        case Phase::Open: {
            auto mkdir = f_mkdir("/wavex/recordings");
            last_result_ = mkdir;
            if (mkdir != FR_OK && mkdir != FR_EXIST) {
                Finish(REC_IO);
                return;
            }
            FILINFO info{};
            for (const char* path: {path_, sidecar_}) {
                auto exists = f_stat(path, &info);
                last_result_ = exists;
                if (exists == FR_OK) {
                    Finish(REC_EXISTS);
                    return;
                }
                if (exists != FR_NO_FILE) {
                    Finish(REC_IO);
                    return;
                }
            }
            auto opened = f_open(&file_, temp_, FA_WRITE | FA_CREATE_NEW);
            last_result_ = opened;
            if (opened != FR_OK) {
                Finish(opened == FR_EXIST ? REC_EXISTS : REC_IO);
                return;
            }
            open_ = wave_owned_ = true;
            phase_ = Phase::Header;
            return;
        }
        case Phase::Header: {
            daisy::WAV_FormatTypeDef header{};
            static_assert(sizeof(header) == 44);
            header.ChunkId = daisy::kWavFileChunkId;
            header.FileSize = bytes_ + 36;
            header.FileFormat = daisy::kWavFileWaveId;
            header.SubChunk1ID = daisy::kWavFileSubChunk1Id;
            header.SubChunk1Size = 16;
            header.AudioFormat = daisy::WAVE_FORMAT_PCM;
            header.NbrChannels = document_.sample.channels;
            header.SampleRate = 48000;
            header.BlockAlign = static_cast<uint16_t>(header.NbrChannels * 2);
            header.ByteRate = 48000u * header.BlockAlign;
            header.BitPerSample = 16;
            header.SubChunk2ID = daisy::kWavFileSubChunk2Id;
            header.SubCHunk2Size = bytes_;
            std::memcpy(block_, &header, sizeof(header));
            if (!Write(block_, sizeof(header))) {
                Finish(REC_IO);
                return;
            }
            phase_ = Phase::Data;
            return;
        }
        case Phase::Data: {
            const uint32_t bytes = std::min<uint32_t>(sizeof(block_), bytes_ - copied_);
            std::memcpy(block_, reinterpret_cast<const uint8_t*>(pcm_) + copied_, bytes);
            if (!Write(block_, bytes)) {
                Finish(REC_IO);
                return;
            }
            for (uint32_t i = 0; i < bytes / 2; ++i) {
                const int32_t v = pcm_[copied_ / 2 + i];
                if (v >= 33 || v <= -33) {
                    const uint32_t frame = (copied_ / 2 + i) / document_.sample.channels;
                    if (first_ == UINT32_MAX)
                        first_ = frame;
                    last_ = frame;
                }
            }
            copied_ += bytes;
            if (copied_ == bytes_) {
                const auto closed = f_close(&file_);
                open_ = false;
                last_result_ = closed;
                if (closed != FR_OK) {
                    Finish(REC_IO);
                    return;
                }
                if (first_ != UINT32_MAX) {
                    auto& m = document_.sample;
                    m.start_frame = first_ > 480 ? first_ - 480 : 0;
                    m.end_frame = std::min(m.total_frames, last_ + 481);
                    m.loop_start = m.start_frame;
                    m.loop_end = m.end_frame;
                }
                phase_ = Phase::Sidecar;
            }
            return;
        }
        case Phase::Sidecar: {
            const auto opened = f_open(&file_, meta_temp_, FA_WRITE | FA_CREATE_NEW);
            last_result_ = opened;
            if (opened != FR_OK) {
                Finish(opened == FR_EXIST ? REC_EXISTS : REC_IO);
                return;
            }
            open_ = meta_owned_ = true;
            if (!SampleFile::Write({this, nullptr, WriteSidecar, nullptr}, document_)) {
                Finish(REC_IO);
                return;
            }
            const auto closed = f_close(&file_);
            open_ = false;
            last_result_ = closed;
            if (closed != FR_OK) {
                Finish(REC_IO);
                return;
            }
            phase_ = Phase::Publish;
            return;
        }
        case Phase::Publish:
            last_result_ = f_rename(meta_temp_, sidecar_);
            if (last_result_ != FR_OK) {
                Finish(REC_IO);
                return;
            }
            meta_owned_ = false;
            sidecar_owned_ = true;
            last_result_ = f_rename(temp_, path_);
            if (last_result_ != FR_OK) {
                Finish(REC_IO);
                return;
            }
            wave_owned_ = false;
            sidecar_owned_ = false;
            Finish(REC_OK);
            return;
    }
}
}  // namespace WaveX::Storage
