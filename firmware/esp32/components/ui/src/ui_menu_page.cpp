// WaveX UI Menu Page Implementation
#include "ui/ui_menu_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "debug/console_command.h"
#include "esp_lvgl_port.h"

// LVGL locking macros
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static const char* TAG = "UI_MENU_PAGE";

namespace wavex_ui {

void UIMenuPage::onEnter(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root_, UI_COLOR_CONTENT, LV_PART_MAIN);  // Dark mode
    lv_obj_set_style_border_width(root_, 0, LV_PART_MAIN);
    lv_obj_align(root_, LV_ALIGN_TOP_LEFT, 0, 0);

    // Title is rendered by navigator header; omit internal title

    list_ = lv_list_create(root_);
    lv_obj_set_size(list_, lv_pct(100), lv_pct(100));
    lv_obj_align(list_, LV_ALIGN_TOP_LEFT, 0, 0);

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

    if (UINavigator::instance().canPop()) {
        keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    }

    keys[1] = {"Select", [this]() { activateSelection(); }};

    return keys;
}

void UIMenuPage::rebuildList() {
    if (!list_)
        return;
    lv_obj_clean(list_);

    for (size_t i = 0; i < items_.size(); ++i) {
        auto item = lv_list_add_btn(list_, LV_SYMBOL_FILE, items_[i].label.c_str());

        lv_obj_set_style_bg_color(item, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN);
        lv_obj_set_style_text_color(item, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(item, UI_FONT_TITLE, LV_PART_MAIN);

        lv_obj_add_event_cb(
            item,
            [](lv_event_t* e) {
                auto* self = static_cast<UIMenuPage*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
                for (int i = 0; i < (int)self->items_.size(); ++i) {
                    if (lv_obj_get_child(self->list_, i) == target) {
                        self->selected_ = i;
                        // Defer activation to avoid modifying object tree during LVGL event
                        // processing
                        lv_async_call(
                            [](void* ud) { static_cast<UIMenuPage*>(ud)->activateSelection(); },
                            self);
                        break;
                    }
                }
            },
            LV_EVENT_SHORT_CLICKED,
            this);

        if ((int)i == selected_) {
            lv_obj_add_state(item, LV_STATE_FOCUSED);
        }
    }
}

// Debug harness: the highlighted item, so a host can steer with the encoder
// from wherever the highlight was left rather than guessing.
size_t UIMenuPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    len = AppendKvInt(out, cap, len, "items", static_cast<long>(items_.size()));
    const bool valid = selected_ >= 0 && selected_ < static_cast<int>(items_.size());
    len = AppendKvInt(out, cap, len, "selidx", valid ? selected_ : -1);
    return AppendKvText(
        out, cap, len, "sel", valid ? items_[static_cast<size_t>(selected_)].label.c_str() : "-");
}

void UIMenuPage::moveSelection(int delta) {
    if (items_.empty())
        return;

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

    // Fallback for a click that reached the list rather than an item. Only an
    // exact match counts: the previous version also accepted any target with
    // LV_OBJ_FLAG_EVENT_BUBBLE set, which is true of the bubbling child of any
    // row, so it selected whichever index the loop happened to be on - item 0
    // in practice, regardless of what was pressed.
    for (int i = 0; i < (int)self->items_.size(); ++i) {
        if (lv_obj_get_child(self->list_, i) != target) {
            continue;
        }
        self->selected_ = i;
        // Deferred for the same reason as the per-item handler above:
        // activating pushes a page, which lv_obj_clean()s this list while its
        // own event is still being dispatched.
        lv_async_call([](void* ud) { static_cast<UIMenuPage*>(ud)->activateSelection(); }, self);
        break;
    }
}

}  // namespace wavex_ui
