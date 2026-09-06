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
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);

    // The bar is the page background, not a panel on top of it: the cards are
    // the only thing that reads as chrome. A rule above the row competes with
    // the shift rule under the header for the same "something changed" signal.
    lv_obj_set_style_bg_color(container_, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(container_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(container_, UI_SOFTKEY_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(container_, UI_SOFTKEY_PAD_BOTTOM, 0);
    lv_obj_set_style_pad_left(container_, UI_SOFTKEY_PAD_X, 0);
    lv_obj_set_style_pad_right(container_, UI_SOFTKEY_PAD_X, 0);
    lv_obj_set_style_pad_column(container_, UI_SOFTKEY_GAP, 0);

    // START, not SPACE_EVENLY: every card grows equally, so the six cells are
    // identical and fixed. SPACE_EVENLY distributes slack around the children,
    // which moves them whenever a label's content width changes.
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < NUM_SOFTKEYS; ++i) {
        btns_[i] = lv_btn_create(container_);
        lv_obj_set_size(btns_[i], LV_SIZE_CONTENT, UI_SOFTKEY_CARD_HEIGHT);
        lv_obj_set_flex_grow(btns_[i], 1);

        // Neutral cards, not accent fills. Six accent blocks along the bottom
        // edge out-shout everything above them, and with no focus concept on
        // this bar there is nothing for accent to single out - the design's
        // full-row renderings (turns 2c/2d) show all six as plain cards.
        lv_obj_set_style_bg_color(btns_[i], UI_COLOR_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            btns_[i],
            UI_COLOR_CARD_ALT,
            LV_PART_MAIN | static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_set_style_border_width(btns_[i], UI_BORDER_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_border_color(btns_[i], UI_COLOR_LINE, LV_PART_MAIN);
        lv_obj_set_style_radius(btns_[i], UI_RADIUS_CARD, LV_PART_MAIN);
        // lv_btn's default theme carries a drop shadow. On an empty slot,
        // whose fill is the page background, the shadow is the only thing that
        // renders - so the row showed five outlines floating below the cards.
        // It is also six software-blurred shadows per frame on a target where
        // that is the expensive operation to avoid.
        lv_obj_set_style_shadow_width(btns_[i], 0, LV_PART_MAIN);
        // Labels are single-line and the cards are fixed width, so a long
        // label must shrink to fit rather than wrap into the card below.
        lv_obj_set_style_pad_hor(btns_[i], UI_PADDING_SMALL, LV_PART_MAIN);

        labels_[i] = lv_label_create(btns_[i]);
        lv_obj_set_style_text_font(labels_[i], UI_FONT_HEADING, LV_PART_MAIN);
        lv_obj_set_style_text_color(labels_[i], UI_COLOR_FG, LV_PART_MAIN);
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
            // The shifted row still tints. With neutral cards that is the
            // only thing distinguishing the two rows at the bar itself, so it
            // matters more here than it did against an accent fill.
            lv_obj_set_style_bg_color(
                btns_[i], shifted ? UI_COLOR_SHIFT : UI_COLOR_CARD, LV_PART_MAIN);
            lv_obj_set_style_border_color(
                btns_[i], shifted ? UI_COLOR_SHIFT : UI_COLOR_LINE, LV_PART_MAIN);
            lv_obj_set_style_text_color(
                labels_[i], shifted ? UI_COLOR_ACCENT_FG : UI_COLOR_FG, LV_PART_MAIN);
        } else {
            lv_obj_clear_flag(btns_[i], LV_OBJ_FLAG_CLICKABLE);
            // An empty slot is background; a disabled one still reads as a key
            // that exists but cannot be used right now. Those are different
            // things and should not look the same.
            lv_obj_set_style_bg_color(btns_[i], empty ? UI_COLOR_BG : UI_COLOR_CARD, LV_PART_MAIN);
            lv_obj_set_style_border_width(btns_[i], empty ? 0 : UI_BORDER_WIDTH, LV_PART_MAIN);
            lv_obj_set_style_border_color(btns_[i], UI_COLOR_LINE, LV_PART_MAIN);
            lv_obj_set_style_text_color(labels_[i], UI_COLOR_DIMMER, LV_PART_MAIN);
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
