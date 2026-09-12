#pragma once

#ifdef UNIT_TEST
#define WAVEX_ITCM_CODE
#define WAVEX_ITCM_CODE_NAMED(name)
#else
#define WAVEX_ITCM_CODE __attribute__((section(".itcm_text")))
// Named subsections separate ordinary functions from header COMDAT groups.
#define WAVEX_ITCM_CODE_NAMED(name) __attribute__((section(".itcm_text." name)))
#endif
#define WAVEX_DTCM_DATA __attribute__((section(".dtcmram_bss")))
#ifdef UNIT_TEST
#define WAVEX_BACKGROUND_DATA
#else
// Raw foreground storage; explicitly constructed after System/SDRAM init.
// Outside the non-cacheable DMA window, never a DMA or callback buffer.
#define WAVEX_BACKGROUND_DATA __attribute__((section(".wavex_background")))
#endif

namespace WaveX {
namespace MemorySections {

// Copies explicitly annotated code from its QSPI load image into ITCM.
// Call once after System::Init and before enabling any IRQ that uses it.
void InitDtcmBss();
void InitItcm();

}  // namespace MemorySections
}  // namespace WaveX
