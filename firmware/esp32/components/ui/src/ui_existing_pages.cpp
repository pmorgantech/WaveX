// WaveX UI Existing Pages Integration Implementation
#include "ui/ui_existing_pages.h"

#include <esp_log.h>

#include "../pages/diagnostics_page.h"
#include "esp_lvgl_port.h"

#include <cstring>

// LVGL locking macros
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_EXISTING_PAGES";

namespace wavex_ui {

// Diagnostics Page Implementation
void UIDiagnosticsPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 1280, 540);
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);  // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create container for diagnostics page
    diagnostics_container_ = lv_obj_create(root_);
    lv_obj_set_size(diagnostics_container_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(
        diagnostics_container_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(diagnostics_container_, 0, LV_PART_MAIN);
    lv_obj_align(diagnostics_container_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create the existing diagnostics page and start its lifecycle
    diagnostics_page_create(diagnostics_container_);
    diagnostics_page_init();

    ESP_LOGI(TAG, "Diagnostics page created in navigation system");
}

void UIDiagnosticsPage::onExit() {
    // Stop diagnostics lifecycle before tearing down UI
    diagnostics_page_stop();
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        diagnostics_container_ = nullptr;
    }
}

void UIDiagnosticsPage::onInput(const InputEvent& evt) {
    // Diagnostics page handles its own input through existing system
    // We just need to handle navigation input
    switch (evt.type) {
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            // Back to system menu
            UINavigator::instance().pop();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UIDiagnosticsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    // Back button
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};

    return keys;
}

// Factory functions
std::shared_ptr<UIPage> createDiagnosticsPage() {
    return std::make_shared<UIDiagnosticsPage>();
}

}  // namespace wavex_ui
