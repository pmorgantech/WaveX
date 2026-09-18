#pragma once
namespace wavex_ui {
// UI domain, with the LVGL lock held. No bus access in these entry points.
void ServicePanelLeds();
void PanelLedWalk();
void PanelLedAll();
void PanelLedTestOff();
// -1 normal policy, -2 all outputs; otherwise the walked physical channel.
int PanelLedTestChannel();
}  // namespace wavex_ui
