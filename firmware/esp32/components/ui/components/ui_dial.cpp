// WaveX dial: a ring plus a readout, for a parameter turned by the encoder
#include "ui_dial.h"

#include "ui_theme.h"

#include <cstdio>

namespace wavex_ui {
namespace {

constexpr int kRing = 132;
constexpr int kRingArcWidth = 14;
constexpr int kPadX = 26;
constexpr int kTextGap = 22;

// Open at the bottom so zero and full scale are visually distinct: a full
// 360-degree ring reads the same at both ends.
constexpr int32_t kArcStart = 135;
constexpr int32_t kArcEnd = 45;

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

Dial dialCreate(lv_obj_t* parent, int x, int y, int w, int h, const char* label) {
    Dial d;

    d.card = lv_obj_create(parent);
    lv_obj_remove_style_all(d.card);
    lv_obj_set_pos(d.card, x, y);
    lv_obj_set_size(d.card, w, h);
    lv_obj_set_style_bg_color(d.card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(d.card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d.card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(d.card, UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(d.card, UI_COLOR_LINE, 0);
    lv_obj_remove_flag(d.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(d.card, LV_OBJ_FLAG_CLICKABLE);

    d.arc = lv_arc_create(d.card);
    lv_obj_set_size(d.arc, kRing, kRing);
    lv_obj_align(d.arc, LV_ALIGN_LEFT_MID, kPadX, 0);
    // No rotation: LVGL measures from 3 o'clock clockwise, so 135 to 405
    // already leaves the gap at the bottom. Rotating by 90 on top of that
    // moved the gap to the top, where it read as a broken ring.
    lv_arc_set_bg_angles(d.arc, kArcStart, 360 + kArcEnd);
    lv_arc_set_range(d.arc, 0, 1000);
    lv_arc_set_value(d.arc, 0);
    // Display-only. Left interactive, a stray touch on the ring would write a
    // parameter the page never asked to change.
    lv_obj_remove_flag(d.arc, LV_OBJ_FLAG_CLICKABLE);
    // The knob has to be flattened in every dimension the theme gives it -
    // removing the style alone left a filled circle riding the arc's end.
    lv_obj_remove_style(d.arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_pad_all(d.arc, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(d.arc, 0, LV_PART_KNOB);
    lv_obj_set_style_arc_width(d.arc, 0, LV_PART_KNOB);
    lv_obj_set_style_arc_width(d.arc, kRingArcWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_width(d.arc, kRingArcWidth, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d.arc, UI_COLOR_CARD_ALT, LV_PART_MAIN);
    lv_obj_set_style_arc_color(d.arc, UI_COLOR_FG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(d.arc, LV_OPA_TRANSP, LV_PART_KNOB);

    // The design puts a percentage inside the ring and a value in real units
    // beside it. We have only the percentage - the engine publishes no mapping
    // from a control value to milliseconds - so printing it in both places
    // would be the same number twice. The ring keeps the angle, the readout
    // keeps the number.
    d.percent = lv_label_create(d.arc);
    lv_label_set_text(d.percent, "");
    lv_obj_set_style_text_font(d.percent, UI_FONT_MONO_SMALL, 0);
    lv_obj_set_style_text_color(d.percent, UI_COLOR_DIM, 0);
    lv_obj_center(d.percent);

    const int text_x = kPadX + kRing + kTextGap;

    d.label = lv_label_create(d.card);
    lv_label_set_text(d.label, label);
    lv_obj_set_style_text_font(d.label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(d.label, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_letter_space(d.label, 1, 0);
    lv_obj_align(d.label, LV_ALIGN_LEFT_MID, text_x, -38);

    d.value = lv_label_create(d.card);
    lv_label_set_text(d.value, "-");
    lv_obj_set_style_text_font(d.value, UI_FONT_MONO_VALUE, 0);
    lv_obj_set_style_text_color(d.value, UI_COLOR_FG, 0);
    lv_obj_align(d.value, LV_ALIGN_LEFT_MID, text_x, 0);

    d.hint = lv_label_create(d.card);
    lv_label_set_text(d.hint, "");
    lv_obj_set_style_text_font(d.hint, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(d.hint, UI_COLOR_DIM, 0);
    lv_obj_align(d.hint, LV_ALIGN_LEFT_MID, text_x, 34);

    return d;
}

void dialSetFocus(Dial& dial, bool focused) {
    if (!dial.card) {
        return;
    }
    lv_obj_set_style_border_width(dial.card, focused ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(dial.card, focused ? UI_COLOR_ACCENT : UI_COLOR_LINE, 0);
    lv_obj_set_style_arc_color(
        dial.arc, focused ? UI_COLOR_ACCENT : UI_COLOR_FG, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(dial.label, focused ? UI_COLOR_ACCENT : UI_COLOR_DIM, 0);
}

void dialSetValue(Dial& dial, float fraction, const char* text, const char* hint) {
    if (!dial.arc) {
        return;
    }
    const float f = clamp01(fraction);
    lv_arc_set_value(dial.arc, static_cast<int32_t>(f * 1000.0f));

    if (text) {
        lv_label_set_text(dial.value, text);
    }
    if (hint) {
        lv_label_set_text(dial.hint, hint);
    }
}

}  // namespace wavex_ui
