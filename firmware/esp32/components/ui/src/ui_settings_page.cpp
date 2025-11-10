// WaveX UI Settings Page Implementation
#include "ui/ui_settings_page.h"

#include <esp_log.h>

#include "esp_lvgl_port.h"

// LVGL locking macros
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_SETTINGS_PAGE";

namespace wavex_ui {

void UISettingsPage::onEnter(lv_obj_t* parent) {
    // NOTE: onEnter is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 480, 320);
    lv_obj_set_style_bg_color(root_, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);  // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Create title
    auto titleLabel = lv_label_create(root_);
    lv_label_set_text(titleLabel, title_.c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 10);

    // Create settings list
    list_ = lv_list_create(root_);
    lv_obj_set_size(list_, 460, 250);
    lv_obj_align(list_, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Dark mode styling for list
    lv_obj_set_style_bg_color(list_, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list_, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);

    rebuildList();
}

void UISettingsPage::onExit() {
    // NOTE: onExit is called from UINavigator::push/pop which already holds LVGL lock
    // No need to acquire lock here
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        list_ = nullptr;
        valueLabels_.clear();
    }
}

void UISettingsPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            if (editingValue_) {
                adjustValue(-1);
            } else {
                moveSelection(-1);
            }
            break;
        case InputType::EncoderRight:
            if (editingValue_) {
                adjustValue(+1);
            } else {
                moveSelection(+1);
            }
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            toggleEditMode();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UISettingsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    // Back button
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};

    // Edit button
    keys[1] = {editingValue_ ? "Done" : "Edit", [this]() { toggleEditMode(); }};

    return keys;
}

void UISettingsPage::rebuildList() {
    if (!list_)
        return;
    LV_LOCK();

    lv_obj_clean(list_);
    valueLabels_.clear();

    for (size_t i = 0; i < settings_.size(); ++i) {
        // Create container for setting name and value
        auto container = lv_obj_create(list_);
        lv_obj_set_size(container, 440, 40);
        lv_obj_set_style_bg_color(container, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN);
        lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);

        // Add to list
        lv_list_add_btn(list_, container, nullptr);

        // Create setting name label
        auto nameLabel = lv_label_create(container);
        lv_label_set_text(nameLabel, settings_[i].label.c_str());
        lv_obj_set_style_text_color(nameLabel, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 10, 0);

        // Create value label
        auto valueLabel = lv_label_create(container);
        char valueText[32];
        snprintf(valueText, sizeof(valueText), "%d", settings_[i].value);
        lv_label_set_text(valueLabel, valueText);
        lv_obj_set_style_text_color(valueLabel, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_MAIN);
        lv_obj_set_style_text_font(valueLabel, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(valueLabel, LV_ALIGN_RIGHT_MID, -10, 0);

        valueLabels_.push_back(valueLabel);

        // Highlight selected setting
        if ((int)i == selectedSetting_) {
            lv_obj_set_style_bg_color(container, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN);
            if (editingValue_) {
                lv_obj_set_style_bg_color(container, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
            }
        }
    }
    LV_UNLOCK();
}

void UISettingsPage::moveSelection(int delta) {
    if (settings_.empty())
        return;

    selectedSetting_ = (selectedSetting_ + delta + settings_.size()) % settings_.size();
    editingValue_ = false;  // Exit edit mode when changing selection
    rebuildList();

    ESP_LOGD(TAG,
             "Selection moved to %d: %s",
             selectedSetting_,
             settings_[selectedSetting_].label.c_str());
}

void UISettingsPage::adjustValue(int delta) {
    if (selectedSetting_ < 0 || selectedSetting_ >= (int)settings_.size())
        return;

    auto& setting = settings_[selectedSetting_];
    int newValue = setting.value + delta;

    // Clamp to min/max range
    if (newValue < setting.minValue)
        newValue = setting.minValue;
    if (newValue > setting.maxValue)
        newValue = setting.maxValue;

    if (newValue != setting.value) {
        updateSetting(selectedSetting_, newValue);
    }
}

void UISettingsPage::toggleEditMode() {
    editingValue_ = !editingValue_;
    rebuildList();

    ESP_LOGD(TAG,
             "Edit mode %s for setting: %s",
             editingValue_ ? "enabled" : "disabled",
             settings_[selectedSetting_].label.c_str());
}

void UISettingsPage::updateSetting(int settingIndex, int newValue) {
    if (settingIndex < 0 || settingIndex >= (int)settings_.size())
        return;

    auto& setting = settings_[settingIndex];
    setting.value = newValue;

    // Update display
    if (settingIndex < (int)valueLabels_.size() && valueLabels_[settingIndex]) {
        LV_LOCK();
        char valueText[32];
        snprintf(valueText, sizeof(valueText), "%d", newValue);
        lv_label_set_text(valueLabels_[settingIndex], valueText);
        LV_UNLOCK();
    }

    // Call change callback
    if (setting.onChange) {
        setting.onChange(newValue);
    }

    ESP_LOGI(TAG, "Setting '%s' changed to %d", setting.label.c_str(), newValue);
}

}  // namespace wavex_ui
