// Process-wide comm interface pointer, set once at startup so UI pages can
// send/receive over the link without depending directly on the comm component.
#include "ui/ui_api.h"

namespace {

WaveX::Comm::ICommInterface* g_comm_interface = nullptr;

}  // namespace

namespace wavex_ui {

void ui_set_comm_interface(WaveX::Comm::ICommInterface* comm_interface) {
    g_comm_interface = comm_interface;
}

WaveX::Comm::ICommInterface* ui_get_comm_interface() {
    return g_comm_interface;
}

}  // namespace wavex_ui
