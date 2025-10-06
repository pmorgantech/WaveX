// WaveX UI Navigation Manager Implementation
#include "ui/ui_navigator.h"
#include "../styles/ui_theme.h"
#include <esp_log.h>

static const char* TAG = "UI_NAVIGATOR";

namespace wavex_ui {

void UINavigator::push(std::shared_ptr<UIPage> page) {
    if (!page) {
        ESP_LOGE(TAG, "Cannot push null page");
        return;
    }

    // Initialize screen and static regions if first page
    if (!screen_) {
        screen_ = lv_obj_create(nullptr);
        lv_scr_load(screen_);
        ESP_LOGI(TAG, "Created navigation screen");

        // Full-screen container background
        lv_obj_set_size(screen_, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(screen_, UI_COLOR_BACKGROUND, LV_PART_MAIN);
        lv_obj_set_style_border_width(screen_, 0, LV_PART_MAIN);

        // Header
        header_ = lv_obj_create(screen_);
        lv_obj_set_size(header_, lv_pct(100), UI_HEADER_HEIGHT);
        lv_obj_set_style_bg_color(header_, UI_COLOR_HEADER, LV_PART_MAIN);
        lv_obj_set_style_border_width(header_, 0, LV_PART_MAIN);
        lv_obj_align(header_, LV_ALIGN_TOP_MID, 0, 0);

        title_label_ = lv_label_create(header_);
        lv_obj_set_style_text_color(title_label_, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(title_label_, UI_FONT_HEADER, LV_PART_MAIN);
        lv_obj_center(title_label_);

        // Content area fills between header and softkeys (we create softkeys later)
        content_ = lv_obj_create(screen_);
        lv_obj_set_size(content_, lv_pct(100), lv_pct(100)); // temporary; corrected after softkey create
        lv_obj_set_style_bg_color(content_, UI_COLOR_CONTENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(content_, 0, LV_PART_MAIN);
        lv_obj_align(content_, LV_ALIGN_TOP_LEFT, 0, UI_HEADER_HEIGHT);
    }

    // Call onExit for current page if any
    if (!stack_.empty()) {
        auto current = stack_.top();
        ESP_LOGI(TAG, "Exiting page: %s", current->name());
        current->onExit();
    }

    // Push new page onto stack
    stack_.push(page);
    active_ = page;

    // Clear content area only and create new page content
    lv_obj_clean(content_);

    // Title
    lv_label_set_text(title_label_, page->name());

    ESP_LOGI(TAG, "Entering page: %s", page->name());
    page->onEnter(content_);

    // Create/update softkey bar and then correct content height
    softkeyBar_.create(screen_);
    softkeyBar_.setSoftkeys(page->getSoftkeys());

    // Compute content height: full screen minus header and softkey heights
    const int16_t total_h = lv_obj_get_height(lv_screen_active());
    const int16_t content_h = total_h - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT;
    lv_obj_set_size(content_, lv_pct(100), content_h > 0 ? content_h : 0);
    lv_obj_align(content_, LV_ALIGN_TOP_LEFT, 0, UI_HEADER_HEIGHT);

    ESP_LOGI(TAG, "Navigation stack depth: %zu", stack_.size());
}

void UINavigator::pop() {
    if (stack_.size() <= 1) {
        ESP_LOGW(TAG, "Cannot pop root page");
        return;
    }

    // Exit current page
    if (!stack_.empty()) {
        auto current = stack_.top();
        ESP_LOGI(TAG, "Exiting page: %s", current->name());
        current->onExit();
        stack_.pop();
    }

    // Get previous page
    if (!stack_.empty()) {
        auto prev = stack_.top();
        active_ = prev;

        // Clear content and recreate previous page content
        lv_obj_clean(content_);
        lv_label_set_text(title_label_, prev->name());

        ESP_LOGI(TAG, "Returning to page: %s", prev->name());
        prev->onEnter(content_);

        // Update softkey bar and layout
        softkeyBar_.create(screen_);
        softkeyBar_.setSoftkeys(prev->getSoftkeys());

        const int16_t total_h = lv_obj_get_height(lv_screen_active());
        const int16_t content_h = total_h - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT;
        lv_obj_set_size(content_, lv_pct(100), content_h > 0 ? content_h : 0);
        lv_obj_align(content_, LV_ALIGN_TOP_LEFT, 0, UI_HEADER_HEIGHT);
    }

    ESP_LOGI(TAG, "Navigation stack depth: %zu", stack_.size());
}

} // namespace wavex_ui
