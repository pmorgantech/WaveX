// WaveX value tile: the shared card for one editable parameter
#pragma once

#include <lvgl.h>

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
struct ValueTile {
    lv_obj_t* card = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* note = nullptr;  // right-hand hint, or the NOT WIRED chip
    lv_obj_t* value = nullptr;
    lv_obj_t* unit = nullptr;
    lv_obj_t* desc = nullptr;  // optional line under the value
    lv_obj_t* bar_track = nullptr;
    lv_obj_t* bar_fill = nullptr;
    int bar_width = 0;  // cached so setFill does not have to measure
};

/// Build a tile at (x, y) sized w x h inside `parent`.
/// `unit` may be nullptr for a bare number.
ValueTile valueTileCreate(
    lv_obj_t* parent, int x, int y, int w, int h, const char* label, const char* unit);

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
