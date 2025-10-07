// WaveX UI Softkey Bar Implementation
#include "ui/ui_softkey_bar.h"
#include "ui/ui_navigator.h"
#include <esp_log.h>
#include "esp_lvgl_port.h"

#include "../styles/ui_theme.h"

// LVGL locking macros
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "SOFTKEY_BAR";

namespace wavex_ui {

void SoftkeyBar::create(lv_obj_t* parent) {

    // Destroy previous container if it exists to avoid duplicates on push/pop
    if (container_) {
        lv_obj_del(container_);
        container_ = nullptr;
    }
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, lv_pct(100), UI_HOTKEY_HEIGHT);
    lv_obj_align(container_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(container_, UI_PADDING_SMALL, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);

    // Set dark mode styling
    lv_obj_set_style_bg_color(container_, UI_COLOR_HOTKEY, LV_PART_MAIN);
    lv_obj_set_style_border_width(container_, UI_BORDER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(container_, UI_COLOR_BORDER, LV_PART_MAIN);

    // Use flex layout for equal distribution
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        btns_[i] = lv_btn_create(container_);
        lv_obj_set_size(btns_[i], LV_SIZE_CONTENT, UI_HOTKEY_HEIGHT - (UI_PADDING_SMALL * 2));
        lv_obj_set_flex_grow(btns_[i], 1);

        // Blue button styling with white text (matching existing UI)
        lv_obj_set_style_bg_color(btns_[i], UI_COLOR_BUTTON, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btns_[i], UI_COLOR_BUTTON_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btns_[i], UI_BORDER_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_border_color(btns_[i], UI_COLOR_BUTTON_BORDER, LV_PART_MAIN);
        lv_obj_set_style_radius(btns_[i], UI_BORDER_RADIUS, LV_PART_MAIN);

        labels_[i] = lv_label_create(btns_[i]);
        lv_obj_set_style_text_font(labels_[i], UI_FONT_HOTKEY, LV_PART_MAIN);
        lv_obj_set_style_text_color(labels_[i], UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_center(labels_[i]);

        // Add event callback
        lv_obj_add_event_cb(btns_[i], event_cb, LV_EVENT_CLICKED, this);
    }
}

void SoftkeyBar::setSoftkeys(const std::array<Softkey, NUM_SOFTKEYS>& keys) {
    keys_ = keys;
    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        lv_label_set_text(labels_[i], keys_[i].label.c_str());
        
        // Update button appearance based on enabled state
        if (keys_[i].enabled) {
            lv_obj_clear_flag(btns_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btns_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(btns_[i], LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_add_flag(btns_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btns_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(btns_[i], LV_OPA_30, LV_PART_MAIN);
        }
    }
}

void SoftkeyBar::focusNext(int delta) {
    focused_ = (focused_ + delta + NUM_SOFTKEYS) % NUM_SOFTKEYS;

    // Update visual focus indication
    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        if (i == focused_ && keys_[i].enabled) {
            lv_obj_add_state(btns_[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_clear_state(btns_[i], LV_STATE_FOCUSED);
        }
    }
}

void SoftkeyBar::pressFocused() {
    if (keys_[focused_].enabled && keys_[focused_].onPress) {
        // Defer to avoid modifying UI during LVGL event processing/draw
        auto cb = keys_[focused_].onPress;
        lv_async_call([](void* ud){
            auto fn = static_cast<std::function<void()>*>(ud);
            (*fn)();
            delete fn;
        }, new std::function<void()>(cb));
    }
}

void SoftkeyBar::event_cb(lv_event_t* e) {
    auto* bar = static_cast<SoftkeyBar*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        if (target == bar->btns_[i] && bar->keys_[i].enabled) {
            ESP_LOGI(TAG, "Softkey %d pressed: %s", i, bar->keys_[i].label.c_str());
            if (bar->keys_[i].onPress) {
                // Defer to avoid modifying UI during LVGL event processing/draw
                auto cb = bar->keys_[i].onPress;
                lv_async_call([](void* ud){
                    auto fn = static_cast<std::function<void()>*>(ud);
                    (*fn)();
                    delete fn;
                }, new std::function<void()>(cb));
            }
            break;
        }
    }
}

} // namespace wavex_ui
