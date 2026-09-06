// WaveX value tile: the shared card for one editable parameter
#pragma once

#include <lvgl.h>

#include <functional>

namespace wavex_ui {

/**
 * @brief A card showing one parameter: name, current value, unit, and fill.
 *
 * This is the design's tile language (turn 2d) as one widget rather than as
 * five near-identical inline copies - Filter, Settings, Sample Edit and Play
 * all want the same card, and the last time a shape like this was written per
 * page the copies drifted apart within a month.
 *
 * The value is set in the mono face so a tile updating at 30 FPS does not
 * change width under its own neighbours.
 *
 * Owns no state and starts no timer: build it in onEnter, keep the returned
 * handle, and call the update functions from wherever the page already
 * refreshes. All of them are LVGL calls, so they must run on the LVGL thread.
 */
/// What a value's own state says about it, independent of focus.
///
/// Focus and meaning were competing for the same fill colour: a tile could say
/// "the encoder is on me" or "this value is boosted", never both. Tone owns the
/// fill whenever it is not Neutral, and focus falls back to the border and
/// label - which is where focus was already legible anyway.
enum class TileTone : uint8_t {
    Neutral,   ///< nothing to say; fill follows focus
    Positive,  ///< above nominal - a boost, or a value doing its job
    Negative,  ///< below nominal - a cut
    Caution,   ///< in a range worth noticing before committing
};

struct ValueTile {
    lv_obj_t* card = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* note = nullptr;  // right-hand hint, or the NOT WIRED chip
    lv_obj_t* value = nullptr;
    lv_obj_t* unit = nullptr;
    lv_obj_t* desc = nullptr;  // optional line under the value
    lv_obj_t* bar_track = nullptr;
    lv_obj_t* bar_fill = nullptr;
    lv_obj_t* knob = nullptr;  ///< rides the fill, so the handle tracks the value
    int bar_width = 0;         // cached so setFill does not have to measure
    /// Chosen at construction from the card's height: a short tile cannot
    /// carry the hero step without its value colliding with the fill bar.
    /// Remembered so setValue() can restore it after a compact string.
    const lv_font_t* value_font = nullptr;
    TileTone tone = TileTone::Neutral;
};

/// Build a tile at (x, y) sized w x h inside `parent`.
/// `unit` may be nullptr for a bare number.
ValueTile valueTileCreate(
    lv_obj_t* parent, int x, int y, int w, int h, const char* label, const char* unit);

/// Colour the fill by what the value means. Survives focus changes, so a
/// boosted gain stays warm whether or not the encoder is on it.
void valueTileSetTone(ValueTile& tile, TileTone tone);

/**
 * @brief Make the tile adjustable by dragging it up and down.
 *
 * A widget that shows a value should let you change it - reading a number you
 * cannot touch, next to a knob that does not turn, teaches the wrong thing
 * about the whole surface. `on_adjust` is called with a signed number of
 * detents as the finger moves.
 *
 * Right and up increase, left and down decrease, and the two axes sum - so a
 * horizontal drag along the fill bar and a vertical drag on the card both
 * work, and a diagonal does the obvious thing rather than being ignored on
 * whichever axis was not committed to.
 *
 * The tile does not scroll or move under the finger: the value changes, the
 * layout does not. The callback should do exactly what the encoder path does,
 * so touch and encoder cannot drift apart.
 */
void valueTileSetOnAdjust(ValueTile& tile, std::function<void(int)> on_adjust);

/// Mark a tile as focused - accent label and a thicker accent border. Exactly
/// one tile in a group should be focused at a time; the caller owns that.
void valueTileSetFocus(ValueTile& tile, bool focused);

/// Set the displayed value text (already formatted, including any sign).
/// `compact` drops to the smaller mono step, for tiles whose value is a name
/// or a word rather than a number - "kick_01" at hero size does not fit a tile
/// and truncating it loses the only part that identifies it.
void valueTileSetValue(ValueTile& tile, const char* text, bool compact = false);

/// Set the line under the value: what the setting means and its range. Cheap
/// to omit - a tile whose label already says everything does not need one.
void valueTileSetDesc(ValueTile& tile, const char* text);

/// Set the fill bar, 0..1. Clamped.
void valueTileSetFill(ValueTile& tile, float fraction);

/// Hide the fill bar, for a tile whose value has no range to sit in. An empty
/// track reads as "zero", which is a different claim from "not a quantity".
void valueTileHideFill(ValueTile& tile);

/**
 * @brief Mark a tile as specified but not yet wired to anything.
 *
 * Draws the value as an em dash, replaces the fill bar with `why`, and puts a
 * NOT WIRED chip in the corner. This exists so a control the protocol cannot
 * carry yet is visibly inert rather than looking broken or, worse, looking
 * like it works - the mistake this codebase has made before.
 */
void valueTileSetUnwired(ValueTile& tile, const char* why, const char* chip = "NOT WIRED");

}  // namespace wavex_ui
