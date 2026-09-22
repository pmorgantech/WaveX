#include "sd_io_diagnostics.hpp"

#include "config/hardware_config.h"
#include "config/logging_config.h"

#include <cstring>

#if WAVEX_DAISY_SD_CARD_ENABLED && (WAVEX_DAISY_SD_CARD_BACKEND == 1)
#include "comm/log_ring.h"
#include "diskio.h"
#include "stm32h7xx_hal.h"
#include "sys/system.h"

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
#if WAVEX_DEBUG_HARNESS_ENABLED
bool bounce_enabled = false;
bool gap_enabled = false;
alignas(32) uint8_t bounce[4096];  // Foreground-owned AXI SRAM, no shared cache lines.
#endif

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
#if WAVEX_DEBUG_HARNESS_ENABLED
uint32_t Experiment() {
    return (bounce_enabled ? 1u : 0u) | (gap_enabled ? 4u : 0u) |
           ((hsd1.Instance->CLKCR & SDMMC_CLKCR_HWFC_EN) ? 2u : 0u);
}
bool ConfigureExperiment(uint32_t mode) {
    if (mode > 7 || hsd1.State != HAL_SD_STATE_READY ||
        (hsd1.Instance->STA & (SDMMC_FLAG_DPSMACT | SDMMC_FLAG_CMDACT)))
        return false;
    bounce_enabled = (mode & 1u) != 0;
    gap_enabled = (mode & 4u) != 0;
    hsd1.Init.HardwareFlowControl =
        (mode & 2u) ? SDMMC_HARDWARE_FLOW_CONTROL_ENABLE : SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    MODIFY_REG(hsd1.Instance->CLKCR, SDMMC_CLKCR_HWFC_EN, hsd1.Init.HardwareFlowControl);
    return true;
}
#endif
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
#if WAVEX_DEBUG_HARNESS_ENABLED
    // Diagnostic only: audio IRQs remain enabled; this is never callback code.
    if (gap_enabled)
        daisy::System::DelayUs(100);
#endif
    const uint32_t start = HAL_GetTick();
    {
        Guard guard;
        capture.Begin(write, drive, sector, count, reinterpret_cast<uint32_t>(buffer));
    }
    auto* dma_buffer = buffer;
#if WAVEX_DEBUG_HARNESS_ENABLED
    if (bounce_enabled && count > 0 && count <= sizeof(bounce) / 512u) {
        dma_buffer = bounce;
        if (write)
            std::memcpy(bounce, buffer, count * 512u);
    }
#endif
    const auto result = write ? __real_disk_write(drive, dma_buffer, sector, count)
                              : __real_disk_read(drive, dma_buffer, sector, count);
    if (!write && result == RES_OK && dma_buffer != buffer)
        std::memcpy(buffer, dma_buffer, count * 512u);
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
