// WaveX dial: a ring plus a readout, for a parameter turned by the encoder
#pragma once

#include <lvgl.h>

namespace wavex_ui {

/**
 * @brief One parameter as a ring, a percentage, a value and a hint.
 *
 * The design's dial (turn 2c) for the envelope stages, where four parameters
 * of the same kind are compared against each other and the ring's angle is
 * the thing being read, not the digits.
 *
 * The ring is an lv_arc with its knob and input removed, not a custom draw:
 * arc is already compiled in, already anti-aliases its own stroke, and
 * redraws only the swept sector when the value changes. A hand-drawn ring
 * would have to justify itself against that with a measurement.
 *
 * Turning is the encoder's job via the owning page, so the arc is display-only
 * - it does not take touch input. Tapping the card to focus it is the page's
 * business, and the card handle is exposed for that.
 */
struct Dial {
    lv_obj_t* card = nullptr;
    lv_obj_t* arc = nullptr;
    lv_obj_t* percent = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* value = nullptr;
    lv_obj_t* hint = nullptr;
};

Dial dialCreate(lv_obj_t* parent, int x, int y, int w, int h, const char* label);

/// Focus: accent ring and label, thicker accent border.
void dialSetFocus(Dial& dial, bool focused);

/// `fraction` drives the ring and the percentage in its middle; `text` is the
/// value in its own units and `hint` the line under it (may be nullptr).
void dialSetValue(Dial& dial, float fraction, const char* text, const char* hint);

}  // namespace wavex_ui
