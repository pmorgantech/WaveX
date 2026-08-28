#include "memory_sections.h"

#include "stm32h7xx.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
extern uint8_t __itcm_text_start__;
extern uint8_t __itcm_text_end__;
extern uint8_t __itcm_text_load__;
}

namespace WaveX {
namespace MemorySections {

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
