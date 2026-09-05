#pragma once

#include "esp_err.h"

#include <cstdint>

namespace wavex_ui {

// Configure and start keypad processing on the BSP I2C bus. Pass the INT pin
// from pin_config.h and the address from hardware_config.h; both are the single
// source of truth and must not be repeated here. The task polls the controller's
// event FIFO and does not read the INT line - see the implementation for why.
esp_err_t tca8418_keypad_start(int int_gpio, uint8_t i2c_addr);

esp_err_t tca8418_keypad_stop();

/// What the matrix last reported, for the Diagnostics ▸ Panel tab. Raw
/// keycodes (1..80, row * 10 + column + 1), before the WAVEX_KEYCODE_* map,
/// so an unmapped key still says where it is.
struct tca8418_keypad_stats_t {
    uint8_t keycode;    ///< last keycode pressed; 0 before the first
    bool pressed;       ///< last transition was a press
    uint32_t events;    ///< presses seen since boot
    uint32_t unmapped;  ///< presses of keycodes with no PanelKey
};
void tca8418_keypad_last(tca8418_keypad_stats_t* out);

}  // namespace wavex_ui
