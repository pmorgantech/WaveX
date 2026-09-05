// WaveX panel LEDs: the logical LEDs of the physical panel
#pragma once

#include "config/hardware_config.h"

#include <cstddef>
#include <cstdint>

namespace wavex_ui {

/**
 * @brief Every LED on the panel, by meaning (panel-controls.md §3.4).
 *
 * What each one shows is the LED policy of §4.5, decided from navigator and
 * page state in one place - no page sets an LED directly. The TLC5947 channel
 * behind each is the WAVEX_LED_CH_* map in hardware_config.h; nothing drives
 * the chain until stage 2.P.3 (`PanelLeds`), this is the model it drives.
 */
enum class PanelLed : uint8_t {
    Soft1 = 0,  ///< dim = softkey defined, bright = latched/active state
    Soft2,
    Soft3,
    Soft4,
    Soft5,
    Soft6,
    Shift,       ///< mirrors UINavigator::isShifted()
    JumpSample,  ///< lit = the active root group
    JumpPlay,
    JumpInstrument,
    JumpTrack,
    JumpMixer,
    JumpSettings,
    PlayStop,  ///< Phase 2 transport semantics
    Rec,
    Pad1,  ///< Phase 2 pad grid: step/playhead mirror
    Pad2,
    Pad3,
    Pad4,
    Pad5,
    Pad6,
    Pad7,
    Pad8,
    Pad9,
    Pad10,
    Pad11,
    Pad12,
    Pad13,
    Pad14,
    Pad15,
    Pad16,
    Count
};

constexpr size_t kPanelLedCount = static_cast<size_t>(PanelLed::Count);

namespace detail {

// Indexed by PanelLed: its TLC5947 channel. The numbers are wiring truth and
// live in hardware_config.h; this only arranges them.
constexpr uint8_t kLedChannels[kPanelLedCount] = {
    WAVEX_LED_CH_SOFT1,         WAVEX_LED_CH_SOFT2,
    WAVEX_LED_CH_SOFT3,         WAVEX_LED_CH_SOFT4,
    WAVEX_LED_CH_SOFT5,         WAVEX_LED_CH_SOFT6,
    WAVEX_LED_CH_SHIFT,         WAVEX_LED_CH_JUMP_SAMPLE,
    WAVEX_LED_CH_JUMP_PLAY,     WAVEX_LED_CH_JUMP_INSTRUMENT,
    WAVEX_LED_CH_JUMP_TRACK,    WAVEX_LED_CH_JUMP_MIXER,
    WAVEX_LED_CH_JUMP_SETTINGS, WAVEX_LED_CH_PLAY_STOP,
    WAVEX_LED_CH_REC,           WAVEX_LED_CH_PAD1,
    WAVEX_LED_CH_PAD2,          WAVEX_LED_CH_PAD3,
    WAVEX_LED_CH_PAD4,          WAVEX_LED_CH_PAD5,
    WAVEX_LED_CH_PAD6,          WAVEX_LED_CH_PAD7,
    WAVEX_LED_CH_PAD8,          WAVEX_LED_CH_PAD9,
    WAVEX_LED_CH_PAD10,         WAVEX_LED_CH_PAD11,
    WAVEX_LED_CH_PAD12,         WAVEX_LED_CH_PAD13,
    WAVEX_LED_CH_PAD14,         WAVEX_LED_CH_PAD15,
    WAVEX_LED_CH_PAD16,
};

constexpr bool ledChannelsInRange() {
    for (size_t i = 0; i < kPanelLedCount; ++i) {
        if (kLedChannels[i] >= WAVEX_LED_CHANNELS) {
            return false;
        }
    }
    return true;
}

constexpr bool ledChannelsUnique() {
    for (size_t i = 0; i < kPanelLedCount; ++i) {
        for (size_t j = i + 1; j < kPanelLedCount; ++j) {
            if (kLedChannels[i] == kLedChannels[j]) {
                return false;
            }
        }
    }
    return true;
}

static_assert(ledChannelsInRange(),
              "a WAVEX_LED_CH_* is >= WAVEX_LED_CHANNELS (hardware_config.h)");
static_assert(ledChannelsUnique(), "two PanelLeds share a TLC5947 channel (hardware_config.h)");

}  // namespace detail

/// TLC5947 channel index (0 .. WAVEX_LED_CHANNELS-1) of a panel LED.
constexpr uint8_t panelLedChannel(PanelLed led) {
    return detail::kLedChannels[static_cast<size_t>(led)];
}

}  // namespace wavex_ui
