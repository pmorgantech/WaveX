// WaveX design palette (Wireframes v2)
#pragma once

#include <cstdint>

// Raw 0xRRGGBB values for use with lv_color_hex().
//
// ui_theme.h carries the older UI_COLOR_* macros (lv_color_t via
// lv_color_make) used by the menu, softkey bar and settings pages. This header
// is the same design's palette in the form the card/tab widgets want, and
// exists because those values were previously private to
// ui_diagnostics_page.cpp - so anything else that wanted to match the look had
// to copy them, which is how two "identical" surfaces drift apart.
//
// Prefer these for new page chrome. The two sets are not in conflict; they are
// the same design expressed for two different LVGL call styles, and unifying
// them is a bigger, riskier edit than any page currently needs.
namespace wavex_ui {
namespace palette {

constexpr uint32_t kColBg = 0x000000;      // page background
constexpr uint32_t kColCard = 0x141414;    // card fill
constexpr uint32_t kColBorder = 0x333333;  // card / row border
constexpr uint32_t kColDim = 0x8FA0AA;     // secondary text
constexpr uint32_t kColDimmer = 0x6E7A82;  // inactive tab label, tertiary text
constexpr uint32_t kColTrack = 0x262B2E;   // gauge track
constexpr uint32_t kColTabOn = 0x10293B;   // selected tab fill
constexpr uint32_t kColGreen = 0x4CAF50;   // ok / selected
constexpr uint32_t kColOrange = 0xFF5722;  // warning / peak
constexpr uint32_t kColBlue = 0x2196F3;    // accent, selected-tab underline

// Card drop shadow (LVGL 9.5 native drop shadow - see docs/roadmap.md 0.3
// item 2). These drive the `drop_shadow_*` style properties, which are a
// different feature from LVGL's older `shadow_*` box-shadow properties.
//
// Values are deliberately restrained. Against kColBg a shadow can only darken
// what is already black, so what reads is the separation at a card's lit
// edges, not a visible drop; a larger radius buys blur cost and no legibility.
constexpr uint32_t kShadowColor = 0x000000;  // shadow tint
constexpr int32_t kShadowRadius = 8;         // blur radius, px
constexpr int32_t kShadowOffsetX = 0;        // straight down, no light angle
constexpr int32_t kShadowOffsetY = 3;        // px
constexpr uint8_t kShadowOpa = 160;          // ~63%

}  // namespace palette
}  // namespace wavex_ui
