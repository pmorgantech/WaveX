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

}  // namespace wavex_ui
