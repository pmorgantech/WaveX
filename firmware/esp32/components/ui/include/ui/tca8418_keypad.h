#pragma once

#include "esp_err.h"

#include <cstdint>

namespace wavex_ui {

// Configure and start keypad processing on the BSP I2C bus. Pass the INT pin
// from pin_config.h and the address from hardware_config.h; both are the single
// source of truth. Call start/stop from the serialized UI lifecycle. IRQ wakes
// the task, with a bounded fallback poll; no I2C or UI work runs in the ISR.
esp_err_t tca8418_keypad_start(int int_gpio, uint8_t i2c_addr);

esp_err_t tca8418_keypad_stop();

/// What the matrix last reported, for the Diagnostics ▸ Panel tab. Raw
/// keycodes (1..80, row * 10 + column + 1), before the WAVEX_KEYCODE_* map,
/// so an unmapped key still says where it is.
struct tca8418_keypad_stats_t {
    uint8_t keycode;     ///< last keycode reported; 0 before the first
    bool pressed;        ///< last transition was a press
    uint32_t events;     ///< presses seen since boot
    bool running;        ///< false for an absent panel or after stop
    bool irq_active;     ///< false if GPIO interrupt setup fell back to polling
    uint32_t io_errors;  ///< failed register passes, including corrupt FIFO data
    uint32_t overflows;  ///< FIFO overflow observations (lost transitions)
    uint32_t unmapped;   ///< presses of keycodes with no PanelKey
};
void tca8418_keypad_last(tca8418_keypad_stats_t* out);

}  // namespace wavex_ui
