// WaveX On-Screen Keyboard / Pad Grid
#include "ui/ui_keyboard_page.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "inter_mcu.h"
#include "ui/ui_navigator.h"

#include <cstdio>

namespace wavex_ui {

namespace {

static const char* TAG = "UI_KEYBOARD";

constexpr int kMidiNoteMax = 127;
// Highest root that still lets all 16 pads land on a real MIDI note.
constexpr int kRootMax = kMidiNoteMax - (UIKeyboardPage::kPads - 1);

const char* const kNoteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Scientific pitch notation, where MIDI 60 is C4.
void NoteName(int note, char* out, size_t len) {
    if (note < 0 || note > kMidiNoteMax) {
        snprintf(out, len, "--");
        return;
    }
    snprintf(out, len, "%s%d", kNoteNames[note % 12], (note / 12) - 1);
}

}  // namespace

void UIKeyboardPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    ui_theme_apply_container_style(root_, true);
    lv_obj_set_style_pad_all(root_, UI_PADDING_MEDIUM, LV_PART_MAIN);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    status_label_ = lv_label_create(root_);
    ui_theme_apply_label_style(status_label_, false);
    lv_obj_set_width(status_label_, lv_pct(100));

    // Four flex rows of four pads. Flex rather than LVGL's grid layout to match
    // how every other container in this UI is built.
    for (int r = 0; r < kRows; ++r) {
        lv_obj_t* row = lv_obj_create(root_);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, UI_PADDING_SMALL, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        for (int c = 0; c < kCols; ++c) {
            const int i = r * kCols + c;
            lv_obj_t* pad = lv_btn_create(row);
            lv_obj_set_height(pad, lv_pct(100));
            lv_obj_set_flex_grow(pad, 1);
            lv_obj_set_style_margin_all(pad, UI_PADDING_SMALL, LV_PART_MAIN);

            lv_obj_set_style_bg_color(pad, UI_COLOR_BUTTON, LV_PART_MAIN);
            lv_obj_set_style_bg_color(pad, UI_COLOR_SELECTED, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_border_width(pad, UI_BORDER_WIDTH, LV_PART_MAIN);
            lv_obj_set_style_border_color(pad, UI_COLOR_BUTTON_BORDER, LV_PART_MAIN);
            lv_obj_set_style_radius(pad, UI_BORDER_RADIUS, LV_PART_MAIN);

            pad_labels_[i] = lv_label_create(pad);
            lv_obj_set_style_text_font(pad_labels_[i], UI_FONT_TITLE, LV_PART_MAIN);
            lv_obj_set_style_text_color(pad_labels_[i], UI_COLOR_TEXT, LV_PART_MAIN);
            lv_obj_center(pad_labels_[i]);

            // A playable key needs press/release, not CLICKED - CLICKED fires
            // on release only, which would make every note zero-length. Gate
            // length follows the finger instead.
            //
            // PRESS_LOST matters as much as RELEASED: a finger that slides off
            // a pad emits PRESS_LOST and no RELEASED, so without it that note
            // would hang until the page exits.
            lv_obj_set_user_data(pad, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_obj_add_event_cb(pad, pad_event_cb, LV_EVENT_PRESSED, this);
            lv_obj_add_event_cb(pad, pad_event_cb, LV_EVENT_RELEASED, this);
            lv_obj_add_event_cb(pad, pad_event_cb, LV_EVENT_PRESS_LOST, this);

            pads_[i] = pad;
            pad_down_[i] = false;
        }
    }

    refreshLabels();
}

void UIKeyboardPage::onExit() {
    // Release anything still held. Leaving the page with a pad down would
    // otherwise strand a Note On with no matching Note Off, and the voice would
    // sustain until it was stolen.
    releaseAll();

    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        status_label_ = nullptr;
        for (int i = 0; i < kPads; ++i) {
            pads_[i] = nullptr;
            pad_labels_[i] = nullptr;
        }
    }
}

std::array<Softkey, NUM_SOFTKEYS> UIKeyboardPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};
    keys[0] = {"Back", []() { UINavigator::instance().pop(); }};
    keys[1] = {"Oct -", [this]() { setRoot(root_note_ - 12); }};
    keys[2] = {"Oct +", [this]() { setRoot(root_note_ + 12); }};
    keys[3] = {"Semi -", [this]() { setRoot(root_note_ - 1); }};
    keys[4] = {"Semi +", [this]() { setRoot(root_note_ + 1); }};
    // Panic. Cheap insurance while this page is the only note source: a stuck
    // voice with no way to release it is otherwise a reboot.
    keys[5] = {"All Off", [this]() { releaseAll(); }};
    return keys;
}

void UIKeyboardPage::pad_event_cb(lv_event_t* e) {
    auto* page = static_cast<UIKeyboardPage*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!page || !target) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    if (index < 0 || index >= kPads) {
        return;
    }

    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            page->press(index);
            break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            page->release(index);
            break;
        default:
            break;
    }
}

void UIKeyboardPage::press(int index) {
    if (pad_down_[index]) {
        return;  // already sounding; don't stack Note Ons
    }
    const int note = root_note_ + index;
    if (note < 0 || note > kMidiNoteMax) {
        return;
    }
    sounding_note_[index] = static_cast<uint8_t>(note);
    pad_down_[index] = true;
    inter_mcu_send_note_on(sounding_note_[index], velocity_, 0);
}

void UIKeyboardPage::release(int index) {
    if (!pad_down_[index]) {
        return;  // no matching press (e.g. RELEASED after PRESS_LOST)
    }
    pad_down_[index] = false;
    inter_mcu_send_note_off(sounding_note_[index], 0);
}

void UIKeyboardPage::releaseAll() {
    for (int i = 0; i < kPads; ++i) {
        release(i);
    }
}

void UIKeyboardPage::setRoot(int root) {
    if (root < 0) {
        root = 0;
    } else if (root > kRootMax) {
        root = kRootMax;
    }
    if (root == root_note_) {
        return;
    }
    // Release first, at the OLD root, so held notes are ended with the note
    // number they were started with.
    releaseAll();
    root_note_ = root;
    refreshLabels();
}

void UIKeyboardPage::refreshLabels() {
    char name[8];
    for (int i = 0; i < kPads; ++i) {
        if (!pad_labels_[i]) {
            continue;
        }
        NoteName(root_note_ + i, name, sizeof(name));
        lv_label_set_text(pad_labels_[i], name);
    }
    if (status_label_) {
        char low[8], high[8];
        NoteName(root_note_, low, sizeof(low));
        NoteName(root_note_ + kPads - 1, high, sizeof(high));
        lv_label_set_text_fmt(status_label_,
                              "%s - %s   vel %d   (load a 16-bit sample first)",
                              low,
                              high,
                              (int)velocity_);
    }
    ESP_LOGI(TAG, "root note %d", root_note_);
}

std::shared_ptr<UIPage> createKeyboardPage() {
    return std::make_shared<UIKeyboardPage>();
}

}  // namespace wavex_ui
