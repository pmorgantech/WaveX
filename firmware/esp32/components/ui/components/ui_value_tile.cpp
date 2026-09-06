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
// The fill is the only part of a tile readable from across a bench, so it is
// worth more height than a hairline progress bar would take.
constexpr int kBarH = 24;
constexpr int kLabelY = kPadY;
constexpr int kValueY = kPadY + 34;

// The value steps down when the card is too short to hold the one above it.
// Without this a 100px tile drew a 48px number straight through its own fill
// bar - the widget knows its own height, so it is the widget's job to pick.
constexpr int kHeroMinHeight = 150;
constexpr int kLargeMinHeight = 120;

const lv_font_t* valueFontFor(int h) {
    if (h >= kHeroMinHeight) {
        return UI_FONT_MONO_HERO;
    }
    if (h >= kLargeMinHeight) {
        return UI_FONT_MONO_LARGE;
    }
    return UI_FONT_MONO_VALUE;
}

constexpr int kKnobW = 12;
constexpr int kKnobH = 32;

// Pixels of vertical travel per detent. Matched to the encoder's feel: a full
// sweep of a tile is roughly a full turn, and a fingertip's worth of movement
// is one step rather than a jump.
constexpr int kDragPixelsPerStep = 9;

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Drag state, one per interactive tile. Heap-allocated and owned by the card
// through its user data, freed on LV_EVENT_DELETE - the ValueTile handle is a
// value type the page copies around, so it cannot own this itself.
struct TileDrag {
    std::function<void(int)> on_adjust;
    int32_t last_x = 0;
    int32_t last_y = 0;
    int32_t carry = 0;  ///< sub-detent travel, kept so slow drags still move
};

void tileDragCb(lv_event_t* e) {
    auto* d = static_cast<TileDrag*>(lv_event_get_user_data(e));
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

    // Right and up both increase, left and down both decrease. Screen y grows
    // downward, so the y term is inverted. Summing the two axes rather than
    // locking to one means a diagonal drag does the obvious thing instead of
    // being ignored on the axis the user did not commit to.
    d->carry += (p.x - d->last_x) + (d->last_y - p.y);
    d->last_x = p.x;
    d->last_y = p.y;
    const int steps = d->carry / kDragPixelsPerStep;
    if (steps != 0) {
        d->carry -= steps * kDragPixelsPerStep;
        d->on_adjust(steps);
    }
}

// Fill colour: tone when the value has something to say, focus otherwise.
lv_color_t fillColour(const ValueTile& tile, bool focused) {
    switch (tile.tone) {
        case TileTone::Positive:
            return UI_COLOR_POSITIVE;
        case TileTone::Negative:
            return UI_COLOR_NEGATIVE;
        case TileTone::Caution:
            return UI_COLOR_WARN;
        case TileTone::Neutral:
        default:
            return focused ? UI_COLOR_ACCENT : UI_COLOR_FG;
    }
}

bool tileFocused(const ValueTile& tile) {
    return tile.card &&
           lv_obj_get_style_border_width(tile.card, LV_PART_MAIN) >= UI_BORDER_WIDTH_FOCUS;
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
    lv_obj_set_style_text_font(t.label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(t.label, UI_COLOR_DIM, 0);
    // Letter-spaced small caps is what makes an 18px label read as a field
    // name rather than as more body text.
    lv_obj_set_style_text_letter_space(t.label, 1, 0);
    lv_obj_set_pos(t.label, kPadX, kLabelY);

    t.value_font = valueFontFor(h);
    t.value = lv_label_create(t.card);
    lv_label_set_text(t.value, "-");
    lv_obj_set_style_text_font(t.value, t.value_font, 0);
    lv_obj_set_style_text_color(t.value, UI_COLOR_FG, 0);
    lv_obj_align(t.value, LV_ALIGN_TOP_MID, 0, kValueY);

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

    // The handle rides the fill's end, so the value is readable as a position
    // and not only as a number. Parented to the card rather than the track so
    // it can overhang the ends without being clipped.
    t.knob = lv_obj_create(t.card);
    lv_obj_remove_style_all(t.knob);
    lv_obj_set_size(t.knob, kKnobW, kKnobH);
    lv_obj_set_pos(t.knob, kPadX - kKnobW / 2, h - kPadY - kBarH - (kKnobH - kBarH) / 2);
    lv_obj_set_style_bg_color(t.knob, UI_COLOR_FG, 0);
    lv_obj_set_style_bg_opa(t.knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t.knob, kKnobW / 2, 0);

    return t;
}

void valueTileSetDesc(ValueTile& tile, const char* text) {
    if (!tile.card) {
        return;
    }
    if (!tile.desc) {
        tile.desc = lv_label_create(tile.card);
        lv_obj_set_style_text_font(tile.desc, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(tile.desc, UI_COLOR_DIM, 0);
        lv_obj_set_pos(tile.desc, kPadX, kValueY + 54);
        // Wrapped rather than clipped: a range like "off - 30 min" is the
        // half that gets cut, and it is the half that answers "what can I set
        // this to".
        lv_obj_set_width(tile.desc, tile.bar_width);  // see valueTileSetValue
        lv_label_set_long_mode(tile.desc, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(tile.desc, text);
}

void valueTileSetFocus(ValueTile& tile, bool focused) {
    if (!tile.card) {
        return;
    }
    lv_obj_set_style_border_width(tile.card, focused ? UI_BORDER_WIDTH_FOCUS : UI_BORDER_WIDTH, 0);
    lv_obj_set_style_border_color(tile.card, focused ? UI_COLOR_ACCENT : UI_COLOR_LINE, 0);
    lv_obj_set_style_text_color(tile.label, focused ? UI_COLOR_ACCENT : UI_COLOR_DIM, 0);
    if (tile.bar_fill) {
        lv_obj_set_style_bg_color(tile.bar_fill, fillColour(tile, focused), 0);
    }
}

// Centres the value, and the value+unit pair when there is a unit - offsetting
// the number by half the unit's width so the group reads as centred rather
// than the number being centred with the unit hanging off it.
void valueTileSetValue(ValueTile& tile, const char* text, bool compact) {
    if (!tile.value) {
        return;
    }
    lv_obj_set_style_text_font(tile.value, compact ? UI_FONT_MONO_VALUE : tile.value_font, 0);
    if (compact) {
        // A compact value can be a sentence rather than a number, so it has to
        // wrap inside the card instead of running off its right edge.
        // tile.bar_width, not a live measurement of the card: at this point
        // the card may not have been laid out, so lv_obj_get_width() returns
        // 0 and the label ends up a negative width - which renders as nothing
        // at all rather than as something visibly wrong.
        lv_obj_set_width(tile.value, tile.bar_width);
        lv_label_set_long_mode(tile.value, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(tile.value, text);

    if (compact) {
        // A wrapped sentence is read left to right, not centred on itself.
        lv_obj_align(tile.value, LV_ALIGN_TOP_LEFT, kPadX, kValueY);
        return;
    }

    int32_t shift = 0;
    if (tile.unit) {
        lv_obj_update_layout(tile.unit);
        shift = (lv_obj_get_width(tile.unit) + UI_PADDING_SMALL) / 2;
    }
    lv_obj_align(tile.value, LV_ALIGN_TOP_MID, -shift, kValueY);
    if (tile.unit) {
        lv_obj_update_layout(tile.value);
        lv_obj_align_to(tile.unit, tile.value, LV_ALIGN_OUT_RIGHT_BOTTOM, UI_PADDING_SMALL, -6);
    }
}

void valueTileSetFill(ValueTile& tile, float fraction) {
    if (!tile.bar_fill) {
        return;
    }
    const int32_t w = static_cast<int32_t>(clamp01(fraction) * static_cast<float>(tile.bar_width));
    lv_obj_set_width(tile.bar_fill, w);
    if (tile.knob) {
        lv_obj_set_x(tile.knob, kPadX + w - kKnobW / 2);
    }
}

void valueTileSetTone(ValueTile& tile, TileTone tone) {
    tile.tone = tone;
    if (tile.bar_fill) {
        lv_obj_set_style_bg_color(tile.bar_fill, fillColour(tile, tileFocused(tile)), 0);
    }
}

void valueTileSetOnAdjust(ValueTile& tile, std::function<void(int)> on_adjust) {
    if (!tile.card) {
        return;
    }
    auto* d = new TileDrag{std::move(on_adjust), 0, 0};
    lv_obj_add_flag(tile.card, LV_OBJ_FLAG_CLICKABLE);
    // Not scrollable and no gesture bubbling: the value moves under the
    // finger, the screen does not.
    lv_obj_remove_flag(tile.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tile.card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(tile.card, tileDragCb, LV_EVENT_PRESSED, d);
    lv_obj_add_event_cb(tile.card, tileDragCb, LV_EVENT_PRESSING, d);
    lv_obj_add_event_cb(tile.card, tileDragCb, LV_EVENT_DELETE, d);
}

void valueTileHideFill(ValueTile& tile) {
    if (tile.bar_track) {
        lv_obj_add_flag(tile.bar_track, LV_OBJ_FLAG_HIDDEN);
    }
    if (tile.knob) {
        lv_obj_add_flag(tile.knob, LV_OBJ_FLAG_HIDDEN);
    }
}

void valueTileSetUnwired(ValueTile& tile, const char* why, const char* chip) {
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
    if (tile.knob) {
        lv_obj_add_flag(tile.knob, LV_OBJ_FLAG_HIDDEN);
    }

    if (!tile.note) {
        tile.note = lv_label_create(tile.card);
        lv_obj_set_style_text_font(tile.note, UI_FONT_MICRO, 0);
        lv_obj_set_style_text_color(tile.note, UI_COLOR_DIM, 0);
        lv_obj_set_style_border_width(tile.note, UI_BORDER_WIDTH, 0);
        lv_obj_set_style_border_color(tile.note, UI_COLOR_LINE, 0);
        lv_obj_set_style_radius(tile.note, UI_RADIUS_BADGE, 0);
        lv_obj_set_style_pad_all(tile.note, 4, 0);
        lv_label_set_text(tile.note, chip);
        lv_obj_align(tile.note, LV_ALIGN_TOP_RIGHT, -kPadX, kPadY - 4);
    }

    lv_obj_t* reason = lv_label_create(tile.card);
    lv_label_set_text(reason, why);
    lv_obj_set_style_text_font(reason, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(reason, UI_COLOR_DIM, 0);
    lv_obj_align(reason, LV_ALIGN_BOTTOM_LEFT, kPadX, -kPadY);
}

}  // namespace wavex_ui
