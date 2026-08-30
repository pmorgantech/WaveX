#pragma once

#define WAVEX_ITCM_CODE __attribute__((section(".itcm_text")))
#define WAVEX_DTCM_DATA __attribute__((section(".dtcmram_bss")))

namespace WaveX {
namespace MemorySections {

// Copies explicitly annotated code from its QSPI load image into ITCM.
// Call once after System::Init and before enabling any IRQ that uses it.
void InitDtcmBss();
void InitItcm();

}  // namespace MemorySections
}  // namespace WaveX
