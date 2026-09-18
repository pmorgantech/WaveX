#pragma once
#include "esp_err.h"

#include "panel/endless_pot.hpp"
namespace wavex_panel {
esp_err_t LoadPotCalibration(WaveX::Panel::Calibration& values);
esp_err_t SavePotCalibration(const WaveX::Panel::Calibration& values);
}  // namespace wavex_panel
