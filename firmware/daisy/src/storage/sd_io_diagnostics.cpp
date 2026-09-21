#include "sd_io_diagnostics.hpp"

#include "config/hardware_config.h"

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
#include "comm/log_ring.h"
#include "diskio.h"
#include "stm32h7xx_hal.h"

extern "C" {
extern SD_HandleTypeDef hsd1;
DRESULT __real_disk_read(BYTE, BYTE*, DWORD, UINT);
DRESULT __real_disk_write(BYTE, const BYTE*, DWORD, UINT);
void __real_HAL_SD_IRQHandler(SD_HandleTypeDef*);
}

namespace WaveX::Storage::SdIo {
namespace {
FirstFailure capture;
bool reported = false;

// PRIMASK restoration also works when called with interrupts already masked.
struct Guard {
    uint32_t saved = __get_PRIMASK();
    Guard() { __disable_irq(); }
    ~Guard() { __set_PRIMASK(saved); }
};

Registers ReadRegisters(const SD_HandleTypeDef& sd) {
    Registers r;
    if (sd.Instance) {
        r.status = sd.Instance->STA;
        r.remaining = sd.Instance->DCOUNT;
        r.dma = sd.Instance->IDMACTRL;
        r.buffer = sd.Instance->IDMABASE0;
        r.clock = sd.Instance->CLKCR;
        r.command = sd.Instance->CMD;
        r.argument = sd.Instance->ARG;
        r.response = sd.Instance->RESP1;
    }
    r.error = sd.ErrorCode;
    r.state = static_cast<uint32_t>(sd.State);
    r.context = sd.Context;
    return r;
}
}  // namespace

// Only Finish publishes first_ and only in foreground context. The IRQ owns
// the in-flight observation, so foreground readers need no interrupt mask.
Failure Snapshot() {
    return capture.Snapshot();
}
void Reset() {
    Guard guard;
    capture.Reset();
    reported = false;
}
void Report() {
    if (reported || !capture.HasFailure())
        return;
    const auto f = Snapshot();
    reported = true;
    WaveX::Log::PrintLine(
        "SDIO_FAIL seq=%lu op=%c drive=%u lba=%lu blocks=%lu buf=%08lx ms=%lu result=%lu irq=%u",
        f.sequence,
        f.write ? 'W' : 'R',
        f.drive,
        f.sector,
        f.count,
        f.buffer,
        f.elapsed_ms,
        f.result,
        f.irq);
    const auto& r = f.registers;
    WaveX::Log::PrintLine(
        "SDIO_REG hal=%08lx state=%lu ctx=%08lx sta=%08lx remain=%lu dma=%08lx base=%08lx "
        "clk=%08lx cmd=%08lx arg=%08lx resp=%08lx",
        r.error,
        r.state,
        r.context,
        r.status,
        r.remaining,
        r.dma,
        r.buffer,
        r.clock,
        r.command,
        r.argument,
        r.response);
}

DRESULT Transfer(bool write, BYTE drive, BYTE* buffer, DWORD sector, UINT count) {
    const uint32_t start = HAL_GetTick();
    {
        Guard guard;
        capture.Begin(write, drive, sector, count, reinterpret_cast<uint32_t>(buffer));
    }
    const auto result = write ? __real_disk_write(drive, buffer, sector, count)
                              : __real_disk_read(drive, buffer, sector, count);
    {
        Guard guard;
        capture.Finish(static_cast<uint32_t>(result), HAL_GetTick() - start, ReadRegisters(hsd1));
    }
    return result;
}

void Irq(SD_HandleTypeDef* sd) {
    // Capture pre-HAL registers: HAL clears flags and disables IDMA in its ISR.
    // Include DATAEND because STOP_TRANSMISSION itself may fail in this branch.
    const auto before = ReadRegisters(*sd);
    __real_HAL_SD_IRQHandler(sd);
    constexpr uint32_t errors = SDMMC_FLAG_DCRCFAIL | SDMMC_FLAG_DTIMEOUT | SDMMC_FLAG_RXOVERR |
                                SDMMC_FLAG_TXUNDERR | SDMMC_FLAG_IDMATE;
    if (sd == &hsd1 && ((before.status & errors) || sd->ErrorCode)) {
        auto failure = before;
        failure.error = sd->ErrorCode;
        capture.ObserveIrq(failure);
    }
}
}  // namespace WaveX::Storage::SdIo

extern "C" DRESULT __wrap_disk_read(BYTE d, BYTE* b, DWORD s, UINT n) {
    return WaveX::Storage::SdIo::Transfer(false, d, b, s, n);
}
extern "C" DRESULT __wrap_disk_write(BYTE d, const BYTE* b, DWORD s, UINT n) {
    return WaveX::Storage::SdIo::Transfer(true, d, const_cast<BYTE*>(b), s, n);
}
extern "C" void __wrap_HAL_SD_IRQHandler(SD_HandleTypeDef* sd) {
    WaveX::Storage::SdIo::Irq(sd);
}
#else
// Keep linker wrapping valid in builds that omit the SDMMC backend.
#include "diskio.h"
#include "stm32h7xx_hal.h"
extern "C" DRESULT __real_disk_read(BYTE, BYTE*, DWORD, UINT);
extern "C" DRESULT __real_disk_write(BYTE, const BYTE*, DWORD, UINT);
extern "C" void __real_HAL_SD_IRQHandler(SD_HandleTypeDef*);
extern "C" DRESULT __wrap_disk_read(BYTE d, BYTE* b, DWORD s, UINT n) {
    return __real_disk_read(d, b, s, n);
}
extern "C" DRESULT __wrap_disk_write(BYTE d, const BYTE* b, DWORD s, UINT n) {
    return __real_disk_write(d, b, s, n);
}
extern "C" void __wrap_HAL_SD_IRQHandler(SD_HandleTypeDef* sd) {
    __real_HAL_SD_IRQHandler(sd);
}
#endif
