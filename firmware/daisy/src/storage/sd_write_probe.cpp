#include "sd_write_probe.hpp"

#include "config/hardware_config.h"

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
#include "sd_sdio.h"
#include "stm32h7xx_hal.h"

#include <algorithm>
#include <cstdio>

namespace WaveX::Storage {
namespace {
uint8_t Pattern(uint32_t offset, uint32_t pattern) {
    if (pattern == 1)
        return 0;
    if (pattern == 2)
        return 0xff;
    if (pattern == 3)
        return (offset & 1u) ? 0x55 : 0xaa;
    // Depends on all offset bytes, so repeated/omitted sectors cannot compare equal.
    uint32_t x = offset + 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return static_cast<uint8_t>(x);
}
}  // namespace
bool SdWriteProbe::Begin(uint32_t bytes,
                         uint32_t chunk,
                         uint32_t shift,
                         uint32_t prefix,
                         uint32_t pattern,
                         uint32_t directory,
                         uint32_t gap_ms) {
    if (Busy() || !SdSdio::IsMounted() || bytes == 0 || bytes > 64u * 1024u * 1024u ||
        (chunk != 512 && chunk != 4096) || (shift != 0 && shift != 4) ||
        (prefix != 0 && prefix != 44) || prefix > bytes || pattern > 3 || directory > 1 ||
        gap_ms > 20)
        return false;
    bytes_ = bytes;
    chunk_ = chunk;
    shift_ = shift;
    prefix_ = prefix;
    pattern_ = pattern;
    directory_ = directory;
    gap_ms_ = gap_ms;
    next_ms_ = HAL_GetTick();
    offset_ = result_ = elapsed_ = 0;
    failed_phase_ = Phase::Idle;
    generation_ = SdSdio::MediaGeneration();
    started_ = HAL_GetTick();
    std::snprintf(path_,
                  sizeof(path_),
                  "%s/wx%06lx.tmp",
                  directory ? "/wavex/recordings" : "",
                  started_ & 0xfffffful);
    phase_ = Phase::Create;
    return true;
}
void SdWriteProbe::Fail(uint32_t result) {
    failed_phase_ = phase_;
    result_ = result;
    phase_ = Phase::Failed;
    elapsed_ = HAL_GetTick() - started_;
    if (open_ && generation_ == SdSdio::MediaGeneration())
        f_close(&file_);
    open_ = false;
}
void SdWriteProbe::Pump() {
    if (!Busy())
        return;
    if (generation_ != SdSdio::MediaGeneration() || !SdSdio::IsMounted()) {
        Fail(FR_NOT_READY);
        return;
    }
    // Defer between application chunks without blocking link/audio service.
    // FatFs may still perform several disk operations within one chunk.
    if (static_cast<int32_t>(HAL_GetTick() - next_ms_) < 0)
        return;
    FRESULT result = FR_OK;
    auto* buffer = buffer_ + shift_;
    switch (phase_) {
        case Phase::Create:
            result = f_open(&file_, path_, FA_WRITE | FA_CREATE_NEW);
            if (result == FR_OK) {
                open_ = true;
                phase_ = Phase::Write;
            }
            break;
        case Phase::Write: {
            const uint32_t count =
                offset_ == 0 && prefix_ ? prefix_ : std::min(chunk_, bytes_ - offset_);
            for (uint32_t i = 0; i < count; ++i)
                buffer[i] = Pattern(offset_ + i, pattern_);
            UINT wrote = 0;
            result = f_write(&file_, buffer, count, &wrote);
            offset_ += wrote;
            if (result == FR_OK && wrote != count) {
                Fail(100);
                return;
            }
            if (result == FR_OK && offset_ == bytes_)
                phase_ = Phase::Sync;
            break;
        }
        case Phase::Sync:
            result = f_sync(&file_);
            if (result == FR_OK)
                phase_ = Phase::CloseWrite;
            break;
        case Phase::CloseWrite:
        case Phase::CloseRead:
            result = f_close(&file_);
            open_ = false;
            if (result == FR_OK)
                phase_ = phase_ == Phase::CloseWrite ? Phase::OpenRead : Phase::Remove;
            break;
        case Phase::OpenRead:
            result = f_open(&file_, path_, FA_READ);
            if (result == FR_OK) {
                open_ = true;
                offset_ = 0;
                phase_ = Phase::Read;
                if (f_size(&file_) != bytes_) {
                    Fail(101);
                    return;
                }
            }
            break;
        case Phase::Read: {
            const uint32_t count = std::min(chunk_, bytes_ - offset_);
            UINT read = 0;
            result = f_read(&file_, buffer, count, &read);
            if (result != FR_OK)
                break;
            if (read != count) {
                Fail(102);
                return;
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (buffer[i] != Pattern(offset_ + i, pattern_)) {
                    offset_ += i;
                    Fail(103);
                    return;
                }
            }
            offset_ += read;
            if (offset_ == bytes_)
                phase_ = Phase::CloseRead;
            break;
        }
        case Phase::Remove:
            result = f_unlink(path_);
            if (result == FR_OK) {
                phase_ = Phase::Done;
                elapsed_ = HAL_GetTick() - started_;
            }
            break;
        default:
            break;
    }
    next_ms_ = HAL_GetTick() + gap_ms_;
    if (result != FR_OK)
        Fail(result);
}
void SdWriteProbe::Status(char* reply, size_t size, int32_t sequence) const {
    std::snprintf(
        reply,
        size,
        "WAVEX-DBG: %ld OK busy=%u phase=%u failed_phase=%u result=%lu bytes=%lu "
        "offset=%lu chunk=%lu shift=%lu prefix=%lu pattern=%lu dir=%lu gap=%lu ms=%lu path=%s",
        static_cast<long>(sequence),
        Busy(),
        static_cast<unsigned>(phase_),
        static_cast<unsigned>(failed_phase_),
        result_,
        bytes_,
        offset_,
        chunk_,
        shift_,
        prefix_,
        pattern_,
        directory_,
        gap_ms_,
        Busy() ? HAL_GetTick() - started_ : elapsed_,
        path_);
}
}  // namespace WaveX::Storage
#endif
