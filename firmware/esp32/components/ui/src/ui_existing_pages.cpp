// WaveX UI Existing Pages Integration Implementation
#include "ui/ui_existing_pages.h"
#include <esp_log.h>

#include "../pages/diagnostics_page.h"
#include "../pages/sample_load_save.h"

static const char* TAG = "UI_EXISTING_PAGES";

namespace wavex_ui {

// Diagnostics Page Implementation
void UIDiagnosticsPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 480, 320);
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create container for diagnostics page
    diagnostics_container_ = lv_obj_create(root_);
    lv_obj_set_size(diagnostics_container_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(diagnostics_container_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(diagnostics_container_, 0, LV_PART_MAIN);
    lv_obj_align(diagnostics_container_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create the existing diagnostics page
    diagnostics_page_create(diagnostics_container_);
    
    ESP_LOGI(TAG, "Diagnostics page created in navigation system");
}

void UIDiagnosticsPage::onExit() {
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

// Sample Load/Save Page Implementation
void UISampleLoadSavePage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 480, 320);
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create container for sample load/save page
    sample_container_ = lv_obj_create(root_);
    lv_obj_set_size(sample_container_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sample_container_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(sample_container_, 0, LV_PART_MAIN);
    lv_obj_align(sample_container_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create the existing sample load/save page
    sample_page_handle_ = wavex_sample_load_save_create(sample_container_);
    
    ESP_LOGI(TAG, "Sample Load/Save page created in navigation system");
}

void UISampleLoadSavePage::onExit() {
    if (sample_page_handle_) {
        wavex_sample_load_save_destroy(static_cast<wavex_sample_load_save_page_t*>(sample_page_handle_));
        sample_page_handle_ = nullptr;
    }
    
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        sample_container_ = nullptr;
    }
}

void UISampleLoadSavePage::onInput(const InputEvent& evt) {
    // Sample load/save page handles its own input through existing system
    // We just need to handle navigation input
    switch (evt.type) {
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            // Back to sample menu
            UINavigator::instance().pop();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISampleLoadSavePage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    
    // Back button
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    
    // Sample-specific actions (if page is active)
    if (sample_page_handle_) {
        auto* page = static_cast<wavex_sample_load_save_page_t*>(sample_page_handle_);
        if (page->is_playing) {
            keys[1] = {"Stop", []() {
                // Stop audition
                ESP_LOGI(TAG, "Stop audition requested");
            }};
        } else {
            keys[1] = {"Audition", []() {
                // Start audition
                ESP_LOGI(TAG, "Audition requested");
            }};
        }
        
        keys[2] = {"Load", []() {
            ESP_LOGI(TAG, "Load sample requested");
        }};
    }
    
    return keys;
}

// Factory functions
std::shared_ptr<UIPage> createDiagnosticsPage() {
    return std::make_shared<UIDiagnosticsPage>();
}

std::shared_ptr<UIPage> createSampleLoadSavePage() {
    return std::make_shared<UISampleLoadSavePage>();
}

} // namespace wavex_ui
