#include "sys/system.h"

#include <cstdint>

// The SRAM debug linker script puts ordinary state in D2, SDMMC-facing state in
// D1 AXI SRAM, and spills a few foreground-only buffers into D3. libDaisy's
// Reset_Handler clears its primary .bss before SystemInit enables D2 SRAM, so
// these SRAM-only ranges are cleared from preinit instead, after SystemInit and
// before global constructors.
extern "C" std::uint32_t __wavex_sram_debug_sd_axi_bss_start__;
extern "C" std::uint32_t __wavex_sram_debug_sd_axi_bss_end__;
extern "C" std::uint32_t __wavex_sram_debug_d2_bss_start__;
extern "C" std::uint32_t __wavex_sram_debug_d2_bss_end__;
extern "C" std::uint32_t __wavex_sram_debug_bss_start__;
extern "C" std::uint32_t __wavex_sram_debug_bss_end__;
extern "C" std::uint32_t __wavex_sram_debug_dtcm_bss_start__;
extern "C" std::uint32_t __wavex_sram_debug_dtcm_bss_end__;
extern "C" std::uint32_t __sram1_bss_start__;
extern "C" std::uint32_t __sram1_bss_end__;

namespace {

void ClearRange(std::uint32_t* current, const std::uint32_t* end) {
    while (current < end) {
        *current++ = 0;
    }
}

void PrepareDirectSwdRuntime() {
    // libDaisy normally interprets an SRAM program as bootloader-launched. A
    // stale pre-v6 bootloader version would make DaisySeed::Init skip clock
    // setup even though direct SWD loading bypassed that bootloader entirely.
    daisy::System::InitBackupSram();
    daisy::boot_info.status = daisy::System::BootInfo::Type::INVALID;
    daisy::boot_info.data = 0;
    daisy::boot_info.version = daisy::System::BootInfo::Version::NONE;

    ClearRange(&__wavex_sram_debug_sd_axi_bss_start__, &__wavex_sram_debug_sd_axi_bss_end__);
    ClearRange(&__sram1_bss_start__, &__sram1_bss_end__);
    ClearRange(&__wavex_sram_debug_d2_bss_start__, &__wavex_sram_debug_d2_bss_end__);
    ClearRange(&__wavex_sram_debug_bss_start__, &__wavex_sram_debug_bss_end__);
    // Constructed state spilled to DTCM: must be zero before the constructors
    // in __libc_init_array run into it (see the linker script's note on why
    // this is not .dtcmram_bss).
    ClearRange(&__wavex_sram_debug_dtcm_bss_start__, &__wavex_sram_debug_dtcm_bss_end__);
}

using PreinitFunction = void (*)();

__attribute__((section(".preinit_array"), used)) PreinitFunction const kPrepareDirectSwdRuntime =
    PrepareDirectSwdRuntime;

}  // namespace
