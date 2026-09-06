// WaveX design palette, raw-integer spelling.
#pragma once

#include "ui_theme.h"

#include <cstdint>

// The same palette as `styles/ui_theme.h`, in the form the card/tab widgets
// want: raw 0xRRGGBB for `lv_color_hex()` rather than `lv_color_t`.
//
// These used to be an independently maintained copy of the design's colours,
// which is how two "identical" surfaces drift apart. They are now derived from
// the `WX_RGB_*` theme tokens, so there is one source of truth and a theme
// switch moves both spellings together. Nothing new should be added here -
// add the role to `ui_theme.h` and, if the raw form is genuinely needed,
// mirror it below.
namespace wavex_ui {
namespace palette {

constexpr uint32_t kColBg = WX_RGB_BG;          // page background
constexpr uint32_t kColCard = WX_RGB_CARD;      // card fill
constexpr uint32_t kColBorder = WX_RGB_LINE;    // card / row border
constexpr uint32_t kColDim = WX_RGB_DIM;        // secondary text
constexpr uint32_t kColDimmer = WX_RGB_DIMMER;  // inactive tab label, tertiary text
constexpr uint32_t kColTrack = WX_RGB_CARD2;    // gauge track
constexpr uint32_t kColTabOn = WX_RGB_CARD2;    // selected tab fill
constexpr uint32_t kColGreen = WX_RGB_OK;       // ok / selected
constexpr uint32_t kColOrange = WX_RGB_WARN;    // warning / peak
constexpr uint32_t kColBlue = WX_RGB_ACC;       // accent, selected-tab underline

// Roles the raw spelling needs that the old hand-copied list did not have.
constexpr uint32_t kColFg = WX_RGB_FG;            // primary text
constexpr uint32_t kColCardAlt = WX_RGB_CARD2;    // inset well
constexpr uint32_t kColAccentFg = WX_RGB_ACC_FG;  // text drawn on the accent
constexpr uint32_t kColShift = WX_RGB_SHIFT;      // shifted-softkey row
constexpr uint32_t kColErr = WX_RGB_ERR;          // failure, not merely attention

}  // namespace palette
}  // namespace wavex_ui
