#pragma once

#include <cstdint>
#include "esp_err.h"

namespace wavex_ui {

// Configure and start keypad processing (uses BSP I2C). INT gpio optional; if <0 polling will be used.
esp_err_t tca8418_keypad_start(int int_gpio /* e.g., 52 */, uint8_t i2c_addr /* 0x34 default */);

// Stop keypad processing and free resources
esp_err_t tca8418_keypad_stop();

}


