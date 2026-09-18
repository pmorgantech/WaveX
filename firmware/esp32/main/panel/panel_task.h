#pragma once
#include "esp_err.h"
#include "ui/panel/panel_led_frame.h"
namespace wavex_panel {
struct Status {
    bool ready = false;
    bool applied = false;
    uint32_t errors = 0, writes = 0;
    wavex_ui::PanelLedFrame frame;
};
// Application lifecycle calls are serialized; stop waits for owned I/O to finish.
esp_err_t Start();
esp_err_t Stop();
// Value copies only: neither caller shares storage with the worker or DMA.
void Publish(const wavex_ui::PanelLedFrame& frame, uint32_t now_ms);
Status ReadStatus();
const char* BackendName();
}  // namespace wavex_panel
