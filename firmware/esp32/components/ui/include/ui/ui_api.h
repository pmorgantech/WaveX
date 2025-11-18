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

void ui_init_demo(lv_obj_t* root);
void ui_set_active_context(std::shared_ptr<UIContext> ctx);

// Dependency injection for UI components
void ui_set_comm_interface(WaveX::Comm::ICommInterface* comm_interface);
WaveX::Comm::ICommInterface* ui_get_comm_interface();

}  // namespace wavex_ui
