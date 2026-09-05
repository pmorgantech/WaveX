// WaveX UI Softkey Bar Implementation
#include "ui/ui_softkey_bar.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "esp_lvgl_port.h"
#include "ui/ui_navigator.h"

// LVGL locking macros
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
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

    lv_obj_set_style_bg_color(container_, UI_COLOR_HOTKEY, LV_PART_MAIN);
    lv_obj_set_style_border_width(container_, UI_BORDER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(container_, UI_COLOR_BORDER, LV_PART_MAIN);

    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        container_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        btns_[i] = lv_btn_create(container_);
        lv_obj_set_size(btns_[i], LV_SIZE_CONTENT, UI_HOTKEY_HEIGHT - (UI_PADDING_SMALL * 2));
        lv_obj_set_flex_grow(btns_[i], 1);

        lv_obj_set_style_bg_color(btns_[i], UI_COLOR_BUTTON, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            btns_[i],
            UI_COLOR_BUTTON_PRESSED,
            LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_set_style_border_width(btns_[i], UI_BORDER_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_border_color(btns_[i], UI_COLOR_BUTTON_BORDER, LV_PART_MAIN);
        lv_obj_set_style_radius(btns_[i], UI_BORDER_RADIUS, LV_PART_MAIN);

        labels_[i] = lv_label_create(btns_[i]);
        lv_obj_set_style_text_font(labels_[i], UI_FONT_HOTKEY, LV_PART_MAIN);
        lv_obj_set_style_text_color(labels_[i], UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_center(labels_[i]);

        lv_obj_add_event_cb(btns_[i], event_cb, LV_EVENT_CLICKED, this);
    }
}

void SoftkeyBar::setSoftkeys(const std::array<Softkey, NUM_SOFTKEYS>& keys, bool shifted) {
    keys_ = keys;
    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        const bool empty = keys_[i].label.empty();
        const bool live = keys_[i].enabled && !empty;

        lv_label_set_text(labels_[i], keys_[i].label.c_str());

        // Every slot stays visible and the same width. Hiding a disabled key
        // collapses the flex row and moves every other key, so muscle memory
        // for "Back is bottom-left" breaks the moment one becomes unavailable.
        lv_obj_clear_flag(btns_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(btns_[i], LV_OPA_COVER, LV_PART_MAIN);

        if (live) {
            lv_obj_add_flag(btns_[i], LV_OBJ_FLAG_CLICKABLE);
            // The shifted row is tinted, so which row is showing is readable
            // from the keys themselves and not only from the Shift indicator.
            lv_obj_set_style_bg_color(
                btns_[i], shifted ? UI_COLOR_BUTTON_SHIFTED : UI_COLOR_BUTTON, LV_PART_MAIN);
            lv_obj_set_style_text_color(labels_[i], UI_COLOR_TEXT, LV_PART_MAIN);
        } else {
            lv_obj_clear_flag(btns_[i], LV_OBJ_FLAG_CLICKABLE);
            // An empty slot is background; a disabled one still reads as a key
            // that exists but cannot be used right now. Those are different
            // things and should not look the same.
            lv_obj_set_style_bg_color(
                btns_[i], empty ? UI_COLOR_HOTKEY : UI_COLOR_BUTTON_DISABLED, LV_PART_MAIN);
            lv_obj_set_style_text_color(labels_[i], UI_COLOR_TEXT_DISABLED, LV_PART_MAIN);
        }
    }
}

bool SoftkeyBar::press(int index) {
    if (index < 0 || index >= NUM_SOFTKEYS || !keys_[index].enabled || keys_[index].label.empty()) {
        return false;
    }
    ESP_LOGI(TAG, "Softkey %d pressed: %s", index, keys_[index].label.c_str());
    // Take the callback BEFORE unsticking Shift: notifySoftkeyUsed() swaps
    // the row back to the unshifted one, and reading keys_[index] after that
    // fires the unshifted key in the same slot - "Track +" became "Unload"
    // (found by the HIL suite, 2026-09-05; the touch path had it all along).
    auto cb = keys_[index].onPress;
    // Before the callback, not after: the callback may push a page, and
    // clearing Shift on the page we just left is the intent.
    UINavigator::instance().notifySoftkeyUsed();
    if (cb) {
        // Defer to avoid modifying UI during LVGL event processing/draw. The
        // panel key path takes this too, so a key and a touch on the same
        // softkey are ordered identically against everything else queued.
        lv_async_call(
            [](void* ud) {
                auto fn = static_cast<std::function<void()>*>(ud);
                (*fn)();
                delete fn;
            },
            new std::function<void()>(cb));
    }
    return true;
}

bool SoftkeyBar::buttonCenter(int index, int32_t* x, int32_t* y) const {
    if (index < 0 || index >= NUM_SOFTKEYS || !btns_[index]) {
        return false;
    }
    lv_obj_update_layout(btns_[index]);
    lv_area_t a;
    lv_obj_get_coords(btns_[index], &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
    return true;
}

void SoftkeyBar::event_cb(lv_event_t* e) {
    auto* bar = static_cast<SoftkeyBar*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        if (target == bar->btns_[i]) {
            bar->press(i);
            break;
        }
    }
}

}  // namespace wavex_ui
