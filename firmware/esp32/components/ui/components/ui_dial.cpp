// WaveX dial: a ring plus a readout, for a parameter turned by the encoder
#include "ui_dial.h"

#include "ui_theme.h"

#include <cstdio>
#include <cstring>

namespace wavex_ui {
namespace {

constexpr int kRing = 132;
constexpr int kRingArcWidth = 14;
constexpr int kPadX = 26;
// Tightened from 22: the readout stepped up a size, so it needs the width back
// from the gap rather than from the card.
constexpr int kTextGap = 16;

// Open at the bottom so zero and full scale are visually distinct: a full
// 360-degree ring reads the same at both ends.
constexpr int32_t kArcStart = 135;
constexpr int32_t kArcEnd = 45;

// Same feel as the value tile's drag, deliberately - one gesture across the
// whole UI, not one per widget.
constexpr int kDragPixelsPerStep = 9;

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

struct DialDrag {
    std::function<void(int)> on_adjust;
    int32_t last_x = 0;
    int32_t last_y = 0;
    int32_t carry = 0;
};

void dialDragCb(lv_event_t* e) {
    auto* d = static_cast<DialDrag*>(lv_event_get_user_data(e));
    if (!d) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        delete d;
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        d->last_x = p.x;
        d->last_y = p.y;
        d->carry = 0;
        return;
    }
    if (code != LV_EVENT_PRESSING || !d->on_adjust) {
        return;
    }
    // Right and up increase, left and down decrease - see ui_value_tile.cpp.
    d->carry += (p.x - d->last_x) + (d->last_y - p.y);
    d->last_x = p.x;
    d->last_y = p.y;
    const int steps = d->carry / kDragPixelsPerStep;
    if (steps != 0) {
        d->carry -= steps * kDragPixelsPerStep;
        d->on_adjust(steps);
    }
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
    lv_obj_set_style_text_font(d.label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(d.label, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_letter_space(d.label, 1, 0);
    lv_obj_align(d.label, LV_ALIGN_LEFT_MID, text_x, -46);

    d.value = lv_label_create(d.card);
    lv_label_set_text(d.value, "-");
    lv_obj_set_style_text_font(d.value, UI_FONT_MONO_HERO, 0);
    lv_obj_set_style_text_color(d.value, UI_COLOR_FG, 0);
    lv_obj_align(d.value, LV_ALIGN_LEFT_MID, text_x, 0);

    d.hint = lv_label_create(d.card);
    lv_label_set_text(d.hint, "");
    lv_obj_set_style_text_font(d.hint, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(d.hint, UI_COLOR_DIM, 0);
    lv_obj_align(d.hint, LV_ALIGN_LEFT_MID, text_x, 46);

    return d;
}

void dialSetOnAdjust(Dial& dial, std::function<void(int)> on_adjust) {
    if (!dial.card) {
        return;
    }
    auto* d = new DialDrag{std::move(on_adjust), 0, 0};
    lv_obj_add_flag(dial.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dial.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dial.card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(dial.card, dialDragCb, LV_EVENT_PRESSED, d);
    lv_obj_add_event_cb(dial.card, dialDragCb, LV_EVENT_PRESSING, d);
    lv_obj_add_event_cb(dial.card, dialDragCb, LV_EVENT_DELETE, d);
}

void dialSetFocus(Dial& dial, bool focused) {
    if (!dial.card) {
        return;
    }
    const int width = focused ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH;
    const auto border = focused ? UI_COLOR_ACCENT : UI_COLOR_LINE;
    const auto arc = focused ? UI_COLOR_ACCENT : UI_COLOR_FG;
    const auto label = focused ? UI_COLOR_ACCENT : UI_COLOR_DIM;
    if (lv_obj_get_style_border_width(dial.card, LV_PART_MAIN) != width)
        lv_obj_set_style_border_width(dial.card, width, 0);
    if (!lv_color_eq(lv_obj_get_style_border_color(dial.card, LV_PART_MAIN), border))
        lv_obj_set_style_border_color(dial.card, border, 0);
    if (!lv_color_eq(lv_obj_get_style_arc_color(dial.arc, LV_PART_INDICATOR), arc))
        lv_obj_set_style_arc_color(dial.arc, arc, LV_PART_INDICATOR);
    if (!lv_color_eq(lv_obj_get_style_text_color(dial.label, LV_PART_MAIN), label))
        lv_obj_set_style_text_color(dial.label, label, 0);
}

void dialSetValue(Dial& dial, float fraction, const char* text, const char* hint) {
    if (!dial.arc) {
        return;
    }
    const float f = clamp01(fraction);
    lv_arc_set_value(dial.arc, static_cast<int32_t>(f * 1000.0f));

    if (text && std::strcmp(lv_label_get_text(dial.value), text)) {
        lv_label_set_text(dial.value, text);
    }
    if (hint && std::strcmp(lv_label_get_text(dial.hint), hint)) {
        lv_label_set_text(dial.hint, hint);
    }
}

}  // namespace wavex_ui
