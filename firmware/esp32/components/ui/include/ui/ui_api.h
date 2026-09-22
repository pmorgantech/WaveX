#pragma once
#include "esp_err.h"

#include <cstdint>

namespace WaveX::Comm {
class ICommInterface;
}
namespace wavex_ui {
struct PanelStatus {
    bool ready = false, applied = false, blanked = true;
    uint32_t errors = 0;
    const char* backend = "Unavailable";
};
// Injected once before UI tasks/listeners start. Application outlives the UI.
// Callbacks return value snapshots; they never share worker/DMA storage.
struct UISharedContext {
    WaveX::Comm::ICommInterface* comm = nullptr;
    esp_err_t (*readEncoder)(uint8_t, int*) = nullptr;
    PanelStatus (*readPanel)() = nullptr;
};
void uiInitialize(const UISharedContext& context);
const UISharedContext& uiContext();
bool takeUIContentChanged();
}  // namespace wavex_ui
// Any ordinary task may signal; UI task consumes atomically.
void wavex_ui_mark_content_changed();
