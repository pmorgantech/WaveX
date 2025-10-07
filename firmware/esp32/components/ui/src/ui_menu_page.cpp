// WaveX UI Menu Page Implementation
#include "ui/ui_menu_page.h"
#include <esp_log.h>
#include "esp_lvgl_port.h"

#include "../styles/ui_theme.h"

// LVGL locking macros
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_MENU_PAGE";

namespace wavex_ui {

void UIMenuPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_CONTENT, LV_PART_MAIN); // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Title is rendered by navigator header; omit internal title

    // Create menu list
    list_ = lv_list_create(root_);
    lv_obj_set_size(list_, lv_pct(100), lv_pct(100));
    lv_obj_align(list_, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Dark mode styling for list
    lv_obj_set_style_bg_color(list_, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list_, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);

    // Keep list click for safety; per-item handlers will do the work
    lv_obj_add_event_cb(list_, list_event_cb, LV_EVENT_CLICKED, this);

    rebuildList();
}

void UIMenuPage::onExit() {
    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        list_ = nullptr;
    }
}

void UIMenuPage::onInput(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            moveSelection(-1);
            break;
        case InputType::EncoderRight:
            moveSelection(+1);
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            activateSelection();
            break;
        default:
            break;
    }
}

std::array<Softkey, NUM_SOFTKEYS> UIMenuPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    
    // Back button (always available if we can pop)
    if (UINavigator::instance().canPop()) {
        keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    }
    
    // Select button
    keys[1] = {"Select", [this]() { activateSelection(); }};
    
    return keys;
}

void UIMenuPage::rebuildList() {
    if (!list_) return;
    lv_obj_clean(list_);

    for (size_t i = 0; i < items_.size(); ++i) {
        auto item = lv_list_add_btn(list_, LV_SYMBOL_FILE, items_[i].label.c_str());
        
        // Dark mode styling for list items
        lv_obj_set_style_bg_color(item, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN);
        lv_obj_set_style_text_color(item, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(item, UI_FONT_TITLE, LV_PART_MAIN);

        // Attach click handler to each item so clicking the row triggers selection
        lv_obj_add_event_cb(item, [](lv_event_t* e){
            auto* self = static_cast<UIMenuPage*>(lv_event_get_user_data(e));
            lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
            // Find index of clicked item
            for (int i = 0; i < (int)self->items_.size(); ++i) {
                if (lv_obj_get_child(self->list_, i) == target) {
                    self->selected_ = i;
                    // Defer activation to avoid modifying object tree during LVGL event processing
                    lv_async_call([](void* ud){
                        static_cast<UIMenuPage*>(ud)->activateSelection();
                    }, self);
                    break;
                }
            }
        }, LV_EVENT_SHORT_CLICKED, this);
        
        // Highlight selected item
        if ((int)i == selected_) {
            lv_obj_add_state(item, LV_STATE_FOCUSED);
        }
    }
}

void UIMenuPage::moveSelection(int delta) {
    if (items_.empty()) return;
    
    selected_ = (selected_ + delta + items_.size()) % items_.size();
    rebuildList();
    
    ESP_LOGD(TAG, "Selection moved to %d: %s", selected_, items_[selected_].label.c_str());
}

void UIMenuPage::activateSelection() {
    if (selected_ >= 0 && selected_ < (int)items_.size()) {
        ESP_LOGI(TAG, "Activating menu item: %s", items_[selected_].label.c_str());
        if (items_[selected_].onSelect) {
            items_[selected_].onSelect();
        }
    }
}

void UIMenuPage::list_event_cb(lv_event_t* e) {
    auto* self = static_cast<UIMenuPage*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    
    // As a fallback, try to map the clicked object to a list item
    for (int i = 0; i < (int)self->items_.size(); ++i) {
        lv_obj_t* item = lv_obj_get_child(self->list_, i);
        if (item == target || lv_obj_has_flag(target, LV_OBJ_FLAG_EVENT_BUBBLE)) {
            self->selected_ = i;
            self->activateSelection();
            break;
        }
    }
}

} // namespace wavex_ui