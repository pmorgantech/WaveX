// WaveX shared busy / progress overlay
#include "ui/ui_busy_overlay.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>

namespace wavex_ui {
namespace BusyOverlay {
namespace {

constexpr uint32_t kColScrim = 0x000000;
constexpr uint32_t kColPanel = 0x141414;
constexpr uint32_t kColBorder = 0x2A2A2A;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColRed = 0xF44336;
constexpr uint32_t kColDim = 0x8FA0AA;

lv_obj_t* s_scrim = nullptr;
lv_obj_t* s_panel = nullptr;
lv_obj_t* s_spinner = nullptr;
lv_obj_t* s_caption = nullptr;
lv_obj_t* s_detail = nullptr;
lv_obj_t* s_bar = nullptr;
lv_timer_t* s_timeout = nullptr;

// Timed out: the operation never answered. Say so and stop spinning, rather
// than leaving a spinner turning forever over a dead backend.
void onTimeout(lv_timer_t*) {
    if (!s_panel || !lv_obj_is_valid(s_panel)) {
        return;
    }
    if (s_spinner && lv_obj_is_valid(s_spinner)) {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_caption && lv_obj_is_valid(s_caption)) {
        lv_label_set_text(s_caption, "No response from backend");
        lv_obj_set_style_text_color(s_caption, lv_color_hex(kColRed), 0);
    }
    if (s_detail && lv_obj_is_valid(s_detail)) {
        lv_label_set_text(s_detail, "Tap to dismiss");
    }
    if (s_timeout) {
        lv_timer_delete(s_timeout);
        s_timeout = nullptr;
    }
}

void onScrimClick(lv_event_t*) {
    // Only dismissable once it has failed. Letting a tap cancel a live
    // operation would leave the backend loading into a UI that has moved on.
    if (!s_timeout) {
        hide();
    }
}

void build() {
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        return;
    }
    s_scrim = lv_obj_create(screen);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(kColScrim), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_70, 0);
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, onScrimClick, LV_EVENT_CLICKED, nullptr);

    s_panel = lv_obj_create(s_scrim);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 640, 260);
    lv_obj_center(s_panel);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(kColPanel), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(s_panel, 6, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_spinner = lv_spinner_create(s_panel);
    lv_obj_set_size(s_spinner, 56, 56);
    lv_obj_set_pos(s_spinner, 32, 40);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(kColGreen), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 5, LV_PART_INDICATOR);

    s_caption = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_caption, lv_color_white(), 0);
    lv_obj_set_pos(s_caption, 112, 40);

    s_detail = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(kColDim), 0);
    lv_obj_set_pos(s_detail, 112, 86);
    lv_obj_set_width(s_detail, 640 - 112 - 32);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);

    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, 640 - 64, 16);
    lv_obj_set_pos(s_bar, 32, 180);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1F1F1F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(kColGreen), LV_PART_INDICATOR);
    // Hidden until setProgress() is called: an indeterminate operation showing
    // a bar stuck at zero reads as "stalled", not as "working".
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void show(const char* caption, const char* detail, uint32_t timeout_ms) {
    if (!s_scrim || !lv_obj_is_valid(s_scrim)) {
        s_scrim = nullptr;
        build();
    }
    if (!s_scrim) {
        return;
    }
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);

    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_caption, caption ? caption : "Working");
    lv_obj_set_style_text_color(s_caption, lv_color_white(), 0);
    lv_label_set_text(s_detail, detail ? detail : "");

    if (s_timeout) {
        lv_timer_delete(s_timeout);
        s_timeout = nullptr;
    }
    if (timeout_ms == 0) {
        timeout_ms = 15000;  // never leave it unbounded, whatever the caller asked
    }
    s_timeout = lv_timer_create(onTimeout, timeout_ms, nullptr);
    lv_timer_set_repeat_count(s_timeout, 1);
}

void setProgress(int percent) {
    if (!s_bar || !lv_obj_is_valid(s_bar)) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);
}

void setDetail(const char* detail) {
    if (s_detail && lv_obj_is_valid(s_detail)) {
        lv_label_set_text(s_detail, detail ? detail : "");
    }
}

void hide() {
    if (s_timeout) {
        lv_timer_delete(s_timeout);
        s_timeout = nullptr;
    }
    if (s_scrim && lv_obj_is_valid(s_scrim)) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
}

bool isVisible() {
    return s_scrim && lv_obj_is_valid(s_scrim) && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace BusyOverlay
}  // namespace wavex_ui
