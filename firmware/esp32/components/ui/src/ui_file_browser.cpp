// WaveX UI File Browser Implementation
#include "ui/ui_file_browser.h"
#include "ui/ui_sample_detail.h"
#include <esp_log.h>
#include "esp_lvgl_port.h"

// LVGL locking macros
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_FILE_BROWSER";

namespace wavex_ui {

void UIFileBrowser::onEnter(lv_obj_t* parent) {
    LV_LOCK();
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 480, 320);
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create path label
    labelPath_ = lv_label_create(root_);
    lv_label_set_text(labelPath_, currentPath_.c_str());
    lv_obj_set_style_text_color(labelPath_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(labelPath_, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(labelPath_, LV_ALIGN_TOP_MID, 0, 8);

    // Create file list
    list_ = lv_list_create(root_);
    lv_obj_set_size(list_, 460, 260);
    lv_obj_align(list_, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    // Dark mode styling for list
    lv_obj_set_style_bg_color(list_, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list_, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);

    // Load files and refresh display
    loadFiles();
    refreshList();
    LV_UNLOCK();
}

void UIFileBrowser::onExit() {
    // Keep state - don't clear currentPath_, files_, or selected_
    // This preserves the browser state when returning to this page
    if (root_) {
        LV_LOCK();
        lv_obj_del(root_);
        LV_UNLOCK();
        root_ = nullptr;
        labelPath_ = nullptr;
        list_ = nullptr;
    }
}

void UIFileBrowser::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            moveSelection(-1);
            break;
        case InputType::EncoderRight:
            moveSelection(+1);
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            openSelection();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UIFileBrowser::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    
    // Back button (always available)
    keys[0] = {"Back", [this]() { UINavigator::instance().pop(); }};
    
    // Open button
    keys[1] = {"Open", [this]() { openSelection(); }};
    
    // Up button (if not at root)
    if (currentPath_ != "/") {
        keys[2] = {"Up", [this]() { navigateUp(); }};
    }
    
    return keys;
}

void UIFileBrowser::refreshList() {
    if (!list_) return;
    LV_LOCK();

    lv_list_clean(list_);
    
    for (size_t i = 0; i < files_.size(); ++i) {
        auto item = lv_list_add_btn(list_, nullptr, files_[i].c_str());
        
        // Dark mode styling for list items
        lv_obj_set_style_bg_color(item, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(item, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(item, &lv_font_montserrat_14, LV_PART_MAIN);
        
        // Highlight selected item
        if ((int)i == selected_) {
            lv_obj_add_state(item, LV_STATE_FOCUSED);
        }
    }
    LV_UNLOCK();
}

void UIFileBrowser::moveSelection(int delta) {
    if (files_.empty()) return;
    
    selected_ = (selected_ + delta + files_.size()) % files_.size();
    refreshList();
    
    ESP_LOGD(TAG, "Selection moved to %d: %s", selected_, files_[selected_].c_str());
}

void UIFileBrowser::openSelection() {
    if (selected_ < 0 || selected_ >= (int)files_.size()) return;
    
    const std::string& selectedFile = files_[selected_];
    ESP_LOGI(TAG, "Opening: %s", selectedFile.c_str());
    
    // Check if it's a directory (simplified - in real implementation, check file attributes)
    if (selectedFile.back() == '/') {
        // Navigate into directory
        std::string newPath = currentPath_;
        if (newPath.back() != '/') newPath += '/';
        newPath += selectedFile.substr(0, selectedFile.length() - 1); // Remove trailing slash
        
        currentPath_ = newPath;
        selected_ = 0; // Reset selection
        loadFiles();
        refreshList();
        
        // Update path label
        if (labelPath_) {
            LV_LOCK();
            lv_label_set_text(labelPath_, currentPath_.c_str());
            LV_UNLOCK();
        }
    } else {
        // Open file - navigate to sample detail page
        UINavigator::instance().push(createSampleDetailPage(selectedFile));
    }
}

void UIFileBrowser::navigateUp() {
    if (currentPath_ == "/") return;
    
    size_t lastSlash = currentPath_.find_last_of('/');
    if (lastSlash == 0) {
        currentPath_ = "/";
    } else {
        currentPath_ = currentPath_.substr(0, lastSlash);
    }
    
    selected_ = 0; // Reset selection
    loadFiles();
    refreshList();
    
    // Update path label
    if (labelPath_) {
        LV_LOCK();
        lv_label_set_text(labelPath_, currentPath_.c_str());
        LV_UNLOCK();
    }
    
    ESP_LOGI(TAG, "Navigated up to: %s", currentPath_.c_str());
}

void UIFileBrowser::loadFiles() {
    files_.clear();
    
    // Simulate file loading - in real implementation, this would read from filesystem
    // For demo purposes, add some sample files
    if (currentPath_ == "/samples") {
        files_ = {"kick.wav", "snare.wav", "hat.wav", "vox1.wav", "bass.wav"};
    } else if (currentPath_ == "/") {
        files_ = {"samples/", "patches/", "settings/"};
    } else {
        files_ = {"sample1.wav", "sample2.wav", "sample3.wav"};
    }
    
    ESP_LOGI(TAG, "Loaded %zu files from %s", files_.size(), currentPath_.c_str());
}

std::shared_ptr<UIPage> createFileBrowserPage(const std::string& path) {
    return std::make_shared<UIFileBrowser>(path);
}

} // namespace wavex_ui
