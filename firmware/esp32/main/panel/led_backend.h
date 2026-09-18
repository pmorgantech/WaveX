#pragma once
#include "esp_err.h"
#include "ui/panel/panel_led_frame.h"
namespace wavex_panel {
// Task context only. init/write/shutdown and all bus resources have exactly
// one owner: panel_task. A successful write means transport completed, not
// that a write-only LED chain's physical wiring has been verified.
struct LedBackend {
    const char* name;
    esp_err_t (*init)();
    esp_err_t (*write)(const wavex_ui::PanelLedFrame&);
    void (*shutdown)();
};
const LedBackend& SelectedLedBackend();
}  // namespace wavex_panel
