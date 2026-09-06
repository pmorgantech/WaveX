// WaveX value tile: the shared card for one editable parameter
#include "ui_value_tile.h"

#include "ui_theme.h"

namespace wavex_ui {
namespace {

// Tile internals, from design turn 2d. The bar sits on the bottom edge inside
// the padding rather than floating, so tiles of different heights still line
// their fills up with each other.
constexpr int kPadX = 20;
constexpr int kPadY = 16;
constexpr int kBarH = 12;
constexpr int kLabelY = kPadY;
constexpr int kValueY = kPadY + 30;

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

ValueTile valueTileCreate(
    lv_obj_t* parent, int x, int y, int w, int h, const char* label, const char* unit) {
    ValueTile t;

    t.card = lv_obj_create(parent);
    lv_obj_remove_style_all(t.card);
    lv_obj_set_pos(t.card, x, y);
    lv_obj_set_size(t.card, w, h);
    lv_obj_set_style_bg_color(t.card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(t.card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t.card, UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(t.card, UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(t.card, UI_COLOR_LINE, 0);
    lv_obj_remove_flag(t.card, LV_OBJ_FLAG_SCROLLABLE);

    t.label = lv_label_create(t.card);
    lv_label_set_text(t.label, label);
    lv_obj_set_style_text_font(t.label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(t.label, UI_COLOR_DIM, 0);
    // Letter-spaced small caps is what makes an 18px label read as a field
    // name rather than as more body text.
    lv_obj_set_style_text_letter_space(t.label, 1, 0);
    lv_obj_set_pos(t.label, kPadX, kLabelY);

    t.value = lv_label_create(t.card);
    lv_label_set_text(t.value, "-");
    lv_obj_set_style_text_font(t.value, UI_FONT_MONO_HERO, 0);
    lv_obj_set_style_text_color(t.value, UI_COLOR_FG, 0);
    lv_obj_set_pos(t.value, kPadX, kValueY);

    if (unit && unit[0]) {
        t.unit = lv_label_create(t.card);
        lv_label_set_text(t.unit, unit);
        lv_obj_set_style_text_font(t.unit, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(t.unit, UI_COLOR_DIM, 0);
        // Aligned to the value rather than positioned absolutely: the value's
        // width changes with its digits even in a mono face.
        lv_obj_align_to(t.unit, t.value, LV_ALIGN_OUT_RIGHT_BOTTOM, UI_PADDING_SMALL, -4);
    }

    t.bar_width = w - 2 * kPadX;
    t.bar_track = lv_obj_create(t.card);
    lv_obj_remove_style_all(t.bar_track);
    lv_obj_set_size(t.bar_track, t.bar_width, kBarH);
    lv_obj_set_pos(t.bar_track, kPadX, h - kPadY - kBarH);
    lv_obj_set_style_bg_color(t.bar_track, UI_COLOR_CARD_ALT, 0);
    lv_obj_set_style_bg_opa(t.bar_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t.bar_track, kBarH / 2, 0);

    t.bar_fill = lv_obj_create(t.bar_track);
    lv_obj_remove_style_all(t.bar_fill);
    lv_obj_set_size(t.bar_fill, 0, kBarH);
    lv_obj_set_pos(t.bar_fill, 0, 0);
    lv_obj_set_style_bg_color(t.bar_fill, UI_COLOR_FG, 0);
    lv_obj_set_style_bg_opa(t.bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t.bar_fill, kBarH / 2, 0);

    return t;
}

void valueTileSetFocus(ValueTile& tile, bool focused) {
    if (!tile.card) {
        return;
    }
    lv_obj_set_style_border_width(tile.card, focused ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(tile.card, focused ? UI_COLOR_ACCENT : UI_COLOR_LINE, 0);
    lv_obj_set_style_text_color(tile.label, focused ? UI_COLOR_ACCENT : UI_COLOR_DIM, 0);
    if (tile.bar_fill) {
        lv_obj_set_style_bg_color(tile.bar_fill, focused ? UI_COLOR_ACCENT : UI_COLOR_FG, 0);
    }
}

void valueTileSetValue(ValueTile& tile, const char* text, bool compact) {
    if (!tile.value) {
        return;
    }
    lv_obj_set_style_text_font(tile.value, compact ? UI_FONT_MONO_VALUE : UI_FONT_MONO_HERO, 0);
    lv_label_set_text(tile.value, text);
    if (tile.unit) {
        lv_obj_update_layout(tile.value);
        lv_obj_align_to(tile.unit, tile.value, LV_ALIGN_OUT_RIGHT_BOTTOM, UI_PADDING_SMALL, -4);
    }
}

void valueTileSetFill(ValueTile& tile, float fraction) {
    if (!tile.bar_fill) {
        return;
    }
    lv_obj_set_width(tile.bar_fill,
                     static_cast<int32_t>(clamp01(fraction) * static_cast<float>(tile.bar_width)));
}

void valueTileSetUnwired(ValueTile& tile, const char* why) {
    if (!tile.card) {
        return;
    }
    // LVGL has no dashed border, which is what the design uses to say
    // "specified but absent". A dimmer border plus the chip carries the same
    // meaning without a custom draw callback for a static decoration.
    lv_obj_set_style_border_color(tile.card, UI_COLOR_DIMMER, 0);
    lv_obj_set_style_text_color(tile.label, UI_COLOR_DIM, 0);
    lv_label_set_text(tile.value, "-");
    lv_obj_set_style_text_color(tile.value, UI_COLOR_DIM, 0);
    if (tile.unit) {
        lv_obj_add_flag(tile.unit, LV_OBJ_FLAG_HIDDEN);
    }
    if (tile.bar_track) {
        lv_obj_add_flag(tile.bar_track, LV_OBJ_FLAG_HIDDEN);
    }

    if (!tile.note) {
        tile.note = lv_label_create(tile.card);
        lv_obj_set_style_text_font(tile.note, UI_FONT_MICRO, 0);
        lv_obj_set_style_text_color(tile.note, UI_COLOR_DIM, 0);
        lv_obj_set_style_border_width(tile.note, UI_BORDER_WIDTH, 0);
        lv_obj_set_style_border_color(tile.note, UI_COLOR_LINE, 0);
        lv_obj_set_style_radius(tile.note, UI_RADIUS_BADGE, 0);
        lv_obj_set_style_pad_all(tile.note, 4, 0);
        lv_label_set_text(tile.note, "NOT WIRED");
        lv_obj_align(tile.note, LV_ALIGN_TOP_RIGHT, -kPadX, kPadY - 4);
    }

    lv_obj_t* reason = lv_label_create(tile.card);
    lv_label_set_text(reason, why);
    lv_obj_set_style_text_font(reason, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reason, UI_COLOR_DIM, 0);
    lv_obj_align(reason, LV_ALIGN_BOTTOM_LEFT, kPadX, -kPadY);
}

}  // namespace wavex_ui
