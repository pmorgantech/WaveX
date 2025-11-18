/**
 * @file ui_api.cpp
 * @brief UI API Implementation
 */

#include "ui/ui_api.h"

#include "ui/input_dispatcher.h"
#include "ui/ui_demo.h"

namespace {

// Global comm interface for UI dependency injection
WaveX::Comm::ICommInterface* g_comm_interface = nullptr;

}  // namespace

namespace wavex_ui {

void ui_init_demo(lv_obj_t* root) {
    // Create patch list page by default
    createPatchListPage(root);
}

void ui_set_active_context(std::shared_ptr<UIContext> ctx) {
    InputDispatcher::instance().setActiveContext(std::move(ctx));
}

// Dependency injection for UI components
void ui_set_comm_interface(WaveX::Comm::ICommInterface* comm_interface) {
    g_comm_interface = comm_interface;
}

WaveX::Comm::ICommInterface* ui_get_comm_interface() {
    return g_comm_interface;
}

}  // namespace wavex_ui
