#include "memory_sections.h"

#include "stm32h7xx.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
extern uint8_t __itcm_text_start__;
extern uint8_t __itcm_text_end__;
extern uint8_t __itcm_text_load__;
extern uint8_t __dtcmram_bss_start__;
extern uint8_t __dtcmram_bss_end__;
}

namespace WaveX {
namespace MemorySections {

// Zero the DTCM .bss.
//
// libDaisy's startup zeroes only _sbss.._ebss, and .dtcmram_bss is a separate
// (NOLOAD) section, so nothing had ever cleared it. Anything placed there with
// WAVEX_DTCM_DATA started as whatever DTCM happened to hold - the previous
// run's data on a warm reset, undefined on a cold one - and its initializer
// was silently discarded.
//
// That is not just untidy. The DTCM voice manager contains callback-visible
// state, pointers, and counters that must start at zero; explicit Init() calls
// then establish every non-zero default before audio starts. This clear must
// therefore run before any DTCM-placed object is initialized or read.
void InitDtcmBss() {
    const size_t size = static_cast<size_t>(&__dtcmram_bss_end__ - &__dtcmram_bss_start__);
    if (size > 0) {
        std::memset(&__dtcmram_bss_start__, 0, size);
        __DSB();
    }
}

void InitItcm() {
    const size_t size = static_cast<size_t>(&__itcm_text_end__ - &__itcm_text_start__);
    if (size > 0) {
        std::memcpy(&__itcm_text_start__, &__itcm_text_load__, size);
        __DSB();
        __ISB();
    }
}

}  // namespace MemorySections
}  // namespace WaveX
