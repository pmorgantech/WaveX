#pragma once
#include "esp_err.h"

#include "panel/endless_pot.hpp"
namespace wavex_panel {
enum class PotCommand : uint8_t { None, Begin, Verify, Save, Disable };
struct PotStatus {
    std::array<uint16_t, 8> raw{};
    std::array<std::array<uint16_t, 2>, 4> wipers{};
    std::array<WaveX::Panel::PotReading, 4> readings{};
    WaveX::Panel::Calibration calibration{};
    WaveX::Panel::PotCalibration candidate{};
    WaveX::Panel::PotCalibrationSession::Stage stage =
        WaveX::Panel::PotCalibrationSession::Stage::Idle;
    int8_t selected = -1;
    uint8_t progress = 0;
    bool adc_ready = false, busy = false;
    uint32_t scans = 0, errors = 0;
    esp_err_t last_result = ESP_OK;
};
// Panel worker owns sampling, decoding and NVS; UI only submits value commands.
void InitPots();
void ServicePots(uint32_t now_ms);
void StopPots();
PotStatus ReadPots();
bool RequestPot(PotCommand command, uint8_t index);
void CancelPotCalibration();
std::array<int16_t, 4> TakePotSteps();
}  // namespace wavex_panel
