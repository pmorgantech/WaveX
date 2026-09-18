#pragma once
#include "ui/panel/panel_led_frame.h"

#include <cstddef>
#include <cstdint>
namespace wavex_panel {
constexpr size_t kTlcFrameBytes = WAVEX_LED_CHANNELS * 12 / 8;
static_assert(WAVEX_LED_CHANNELS % 24 == 0, "TLC5947 needs complete 24-channel devices");
inline void EncodeTlc5947(const wavex_ui::PanelLedFrame& frame, uint8_t* out) {
    // Last chained channel first, each 12-bit grayscale value MSB first.
    for (size_t pair = 0; pair < WAVEX_LED_CHANNELS / 2; ++pair) {
        const size_t channel = WAVEX_LED_CHANNELS - 1 - pair * 2;
        const auto a =
            (uint32_t(wavex_ui::PanelLedChannelLevel(frame, channel)) * 4095 + 127) / 255;
        const auto b =
            (uint32_t(wavex_ui::PanelLedChannelLevel(frame, channel - 1)) * 4095 + 127) / 255;
        out[pair * 3] = static_cast<uint8_t>(a >> 4);
        out[pair * 3 + 1] = static_cast<uint8_t>((a << 4) | (b >> 8));
        out[pair * 3 + 2] = static_cast<uint8_t>(b);
    }
}
}  // namespace wavex_panel
