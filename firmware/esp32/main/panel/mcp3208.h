#pragma once
#include "esp_err.h"

#include <array>
#include <cstdint>
namespace wavex_panel {
esp_err_t InitAdc();
esp_err_t ReadAdc(std::array<uint16_t, 8>& values);
void CloseAdc();
}  // namespace wavex_panel
