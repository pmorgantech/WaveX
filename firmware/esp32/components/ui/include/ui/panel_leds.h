#pragma once
#include "esp_err.h"
#include "ui/pca9956b_controller.h"

namespace wavex_ui {
struct PanelLedStats {
    Pca9956bController::State state = Pca9956bController::State::Off;
    uint32_t frames = 0;
    uint32_t errors = 0;
    uint8_t fault_status = 0;
};
// UI task requests lifecycle changes; all GPIO/device ownership stays in panel_task.
void panel_leds_start();
esp_err_t panel_leds_stop();
void panel_leds_publish_ui();
PanelLedStats panel_leds_stats();
// Called only by the existing PCNT worker, now panel_task.
void panel_leds_service();
void panel_leds_shutdown();
}  // namespace wavex_ui
