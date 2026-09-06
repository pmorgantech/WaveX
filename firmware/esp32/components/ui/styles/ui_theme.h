/**
 * @file ui_theme.h
 * @brief The single source of truth for UI colour, type and chrome geometry.
 *
 * Every colour, font and fixed dimension the UI draws with is named here.
 * Page code must not contain literal colours, font pointers or pixel
 * constants - a palette, type-ladder or chrome change has to land in one file,
 * and that is only true if nothing bypasses this header.
 *
 * ## Themes
 *
 * The palette is chosen at compile time from `themes/ui_theme_<name>.h`. Set
 * it with `-DWAVEX_UI_THEME=<neutral|amber|teal|contrast>` at configure time;
 * `components/ui/CMakeLists.txt` validates the name and turns it into the
 * `WAVEX_UI_THEME_*` define read below. Themes only re-map the raw
 * `WX_RGB_*` values; no layout, font or geometry differs between them, so a
 * page that draws correctly in one draws correctly in all four.
 *
 * ## Two spellings of the same palette
 *
 * `UI_COLOR_*` are `lv_color_t` for the `lv_obj_set_style_*_color()` call
 * style; `wavex_ui::palette::kCol*` in `ui/ui_palette.h` are raw `uint32_t`
 * for the `lv_color_hex()` call style. Both now derive from the `WX_RGB_*`
 * values below, so the two surfaces can no longer drift apart - which they
 * previously could, and did.
 */

#ifndef WAVEX_UI_THEME_H
#define WAVEX_UI_THEME_H

#include "lvgl.h"
#include "wavex_fonts.h"

/* Avoid broad extern "C" to prevent C linkage on C++ includes */

// ---------------------------------------------------------------------------
// Theme selection
// ---------------------------------------------------------------------------
#if defined(WAVEX_UI_THEME_AMBER)
#include "themes/ui_theme_amber.h"
#elif defined(WAVEX_UI_THEME_TEAL)
#include "themes/ui_theme_teal.h"
#elif defined(WAVEX_UI_THEME_CONTRAST)
#include "themes/ui_theme_contrast.h"
#else
#include "themes/ui_theme_neutral.h"
#endif

// ---------------------------------------------------------------------------
// Colour roles
//
// Prefer a role over a raw WX_RGB_* value: the role is what the design means,
// and is what stays correct when a theme redefines the underlying colour.
// ---------------------------------------------------------------------------
#define UI_COLOR_BG lv_color_hex(WX_RGB_BG)             // page background
#define UI_COLOR_CARD lv_color_hex(WX_RGB_CARD)         // card / header fill
#define UI_COLOR_CARD_ALT lv_color_hex(WX_RGB_CARD2)    // inset well, gauge track
#define UI_COLOR_LINE lv_color_hex(WX_RGB_LINE)         // 1px card border, rules
#define UI_COLOR_FG lv_color_hex(WX_RGB_FG)             // primary text
#define UI_COLOR_DIM lv_color_hex(WX_RGB_DIM)           // secondary text
#define UI_COLOR_DIMMER lv_color_hex(WX_RGB_DIMMER)     // tertiary / inactive text
#define UI_COLOR_ACCENT lv_color_hex(WX_RGB_ACC)        // focus, selection, active tab
#define UI_COLOR_ACCENT_FG lv_color_hex(WX_RGB_ACC_FG)  // text drawn on the accent
#define UI_COLOR_ACCENT_PRESSED lv_color_hex(WX_RGB_ACC_PRESSED)
#define UI_COLOR_OK lv_color_hex(WX_RGB_OK)        // bound / healthy / meter
#define UI_COLOR_WARN lv_color_hex(WX_RGB_WARN)    // attention, peak
#define UI_COLOR_SHIFT lv_color_hex(WX_RGB_SHIFT)  // shifted-softkey row
#define UI_COLOR_ERR lv_color_hex(WX_RGB_ERR)      // failure, not merely attention

// Legacy spellings, retained so pages not yet restyled keep building. They are
// aliases, not a second palette - do not add to this list, and prefer the role
// names above in new code.
#define UI_COLOR_BACKGROUND UI_COLOR_BG
#define UI_COLOR_HEADER UI_COLOR_CARD
#define UI_COLOR_CONTENT UI_COLOR_BG
#define UI_COLOR_HOTKEY UI_COLOR_BG
#define UI_COLOR_BORDER UI_COLOR_LINE
#define UI_COLOR_TEXT UI_COLOR_FG
#define UI_COLOR_BUTTON UI_COLOR_ACCENT
#define UI_COLOR_BUTTON_PRESSED UI_COLOR_ACCENT_PRESSED
#define UI_COLOR_BUTTON_SHIFTED UI_COLOR_SHIFT
#define UI_COLOR_BUTTON_DISABLED UI_COLOR_CARD_ALT
#define UI_COLOR_TEXT_DISABLED UI_COLOR_DIMMER
#define UI_COLOR_BUTTON_BORDER UI_COLOR_ACCENT_PRESSED
#define UI_COLOR_SELECTED UI_COLOR_OK
#define UI_COLOR_METER UI_COLOR_OK
#define UI_COLOR_PEAK UI_COLOR_WARN

// ---------------------------------------------------------------------------
// Type ladder
//
// Five Montserrat steps for prose and four JetBrains Mono steps for values.
// These are the only sizes compiled in (`CONFIG_LV_FONT_MONTSERRAT_*` in
// sdkconfig.defaults, plus fonts/wavex_mono_*.c) - naming any other size is a
// link error, which is deliberate. Adding a step means adding a font table, so
// snap to the nearest existing step instead unless the design genuinely needs
// a new one.
//
// Use the mono steps for anything read as a number: they are tabular, so a
// value updating at 30 FPS does not shift the widgets beside it.
// ---------------------------------------------------------------------------
#define UI_FONT_MICRO &lv_font_montserrat_14    // chips, badges, axis ticks
#define UI_FONT_SMALL &lv_font_montserrat_18    // hints, secondary lines
#define UI_FONT_BODY &lv_font_montserrat_22     // body text, tab labels
#define UI_FONT_TITLE &lv_font_montserrat_26    // section and card titles
#define UI_FONT_HEADING &lv_font_montserrat_30  // page title, row titles, softkeys

#define UI_FONT_MONO_MICRO &wavex_mono_14  // CPU %, tiny counters
#define UI_FONT_MONO_SMALL &wavex_mono_18  // inline values, context strings
#define UI_FONT_MONO_VALUE &wavex_mono_26  // tile values
#define UI_FONT_MONO_HERO &wavex_mono_38   // the one value a page is about

// Legacy spellings; aliases only. UI_FONT_TITLE kept its old size (26).
#define UI_FONT_NORMAL UI_FONT_SMALL
#define UI_FONT_HEADER UI_FONT_HEADING
#define UI_FONT_HOTKEY UI_FONT_HEADING

// ---------------------------------------------------------------------------
// Chrome geometry
//
// The panel is 720x1280 rotated 90 degrees in software, so the UI draws to
// 1280x720. Header, shift rule and softkey bar are fixed on every page; what
// is left is the content area, and a page may not draw outside it.
// ---------------------------------------------------------------------------
#define UI_SCREEN_WIDTH 1280
#define UI_SCREEN_HEIGHT 720

#define UI_HEADER_HEIGHT 64     // title, context, meter, CPU, SHIFT chip
#define UI_SHIFT_RULE_HEIGHT 3  // under the header; shift-coloured when latched
#define UI_HOTKEY_HEIGHT 96     // softkey bar, including its padding

#define UI_CONTENT_TOP (UI_HEADER_HEIGHT + UI_SHIFT_RULE_HEIGHT)                  // 67
#define UI_CONTENT_HEIGHT (UI_SCREEN_HEIGHT - UI_CONTENT_TOP - UI_HOTKEY_HEIGHT)  // 557
#define UI_CONTENT_WIDTH UI_SCREEN_WIDTH

// Softkey bar internals. Six equal cards, never fewer - an unavailable action
// leaves its cell empty so the row cannot reflow and a key never moves under
// the user's finger.
#define UI_SOFTKEY_COUNT 6
#define UI_SOFTKEY_GAP 8
#define UI_SOFTKEY_PAD_X 20
#define UI_SOFTKEY_PAD_TOP 8
#define UI_SOFTKEY_PAD_BOTTOM 12
#define UI_SOFTKEY_CARD_HEIGHT \
    (UI_HOTKEY_HEIGHT - UI_SOFTKEY_PAD_TOP - UI_SOFTKEY_PAD_BOTTOM)  // 76

// Shared spacing and shape.
#define UI_MARGIN_X 20  // content inset from the screen edge
#define UI_GUTTER 8     // between sibling cards
#define UI_RADIUS_CARD 10
#define UI_RADIUS_CHIP 8
#define UI_RADIUS_BADGE 5
#define UI_BORDER_WIDTH 1  // cards carry a hairline, not a 2px slab
#define UI_BORDER_WIDTH_FOCUS 2
#define UI_BORDER_RADIUS UI_RADIUS_CARD  // legacy alias
#define UI_PADDING_SMALL 5
#define UI_PADDING_MEDIUM 10
#define UI_PADDING_LARGE 15

// Header internals. Title and context read left; the meter, engine-CPU
// readout and SHIFT chip are anchored right, in that order. Both the status
// strip and the navigator lay out against these, so the two cannot disagree
// about where the chip starts.
#define UI_HEADER_GAP 18
#define UI_SHIFT_CHIP_W 100
#define UI_SHIFT_CHIP_H 40
#define UI_SHIFT_CHIP_Y ((UI_HEADER_HEIGHT - UI_SHIFT_CHIP_H) / 2)
#define UI_SHIFT_CHIP_X (UI_SCREEN_WIDTH - UI_MARGIN_X - UI_SHIFT_CHIP_W)

void ui_theme_apply_button_style(lv_obj_t* button, bool is_pressed_style);
void ui_theme_apply_container_style(lv_obj_t* container, bool has_border);
void ui_theme_apply_label_style(lv_obj_t* label, bool is_title);

#endif  // WAVEX_UI_THEME_H
