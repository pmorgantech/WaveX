#pragma once
#include <cstdint>
namespace wavex_ui {
// Optional page presentation; no physical channels or peripheral dependencies.
struct PanelPageLeds {
    uint16_t defined = 0, active = 0;
    bool recording = false;
};
}  // namespace wavex_ui
