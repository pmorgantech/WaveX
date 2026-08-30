#pragma once

#include "lvgl.h"
#include "ui/ui_context.h"

#include <memory>

namespace WaveX {
namespace Comm {
class ICommInterface;
}
}  // namespace WaveX

namespace wavex_ui {

// Dependency injection for UI components
void ui_set_comm_interface(WaveX::Comm::ICommInterface* comm_interface);
WaveX::Comm::ICommInterface* ui_get_comm_interface();

}  // namespace wavex_ui
