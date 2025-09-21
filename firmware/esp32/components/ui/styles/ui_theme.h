/**
 * @file ui_theme.h
 * @brief UI Theme and Styling Definitions
 * 
 * This file contains centralized theme definitions for consistent UI styling
 * across all WaveX UI components.
 */

#ifndef WAVEX_UI_THEME_H
#define WAVEX_UI_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Color definitions for dark theme
#define UI_COLOR_BACKGROUND      lv_color_make(0x00, 0x00, 0x00)  // Black
#define UI_COLOR_HEADER          lv_color_make(0x2E, 0x34, 0x40)  // Dark gray
#define UI_COLOR_CONTENT         lv_color_make(0x00, 0x00, 0x00)  // Black
#define UI_COLOR_HOTKEY          lv_color_make(0x00, 0x00, 0x00)  // Black
#define UI_COLOR_BORDER          lv_color_make(0x33, 0x33, 0x33)  // Dark gray
#define UI_COLOR_TEXT            lv_color_make(0xFF, 0xFF, 0xFF)  // White
#define UI_COLOR_BUTTON          lv_color_make(0x21, 0x96, 0xF3)  // Blue
#define UI_COLOR_BUTTON_PRESSED  lv_color_make(0x19, 0x76, 0xD2)  // Darker blue
#define UI_COLOR_BUTTON_BORDER   lv_color_make(0x15, 0x65, 0xC0)  // Darker blue
#define UI_COLOR_SELECTED        lv_color_make(0x4C, 0xAF, 0x50)  // Green
#define UI_COLOR_METER           lv_color_make(0x4C, 0xAF, 0x50)  // Green
#define UI_COLOR_PEAK            lv_color_make(0xFF, 0x57, 0x22)  // Orange

// Font definitions
#define UI_FONT_NORMAL           &lv_font_montserrat_14
#define UI_FONT_TITLE            &lv_font_montserrat_22
#define UI_FONT_HEADER           &lv_font_montserrat_32
#define UI_FONT_HOTKEY           &lv_font_montserrat_36

// Size definitions
#define UI_HEADER_HEIGHT         75
#define UI_HOTKEY_HEIGHT         100
#define UI_BORDER_WIDTH          2
#define UI_BORDER_RADIUS         5
#define UI_PADDING_SMALL         5
#define UI_PADDING_MEDIUM        10
#define UI_PADDING_LARGE         15

// Button styling function
void ui_theme_apply_button_style(lv_obj_t* button, bool is_pressed_style);

// Container styling function
void ui_theme_apply_container_style(lv_obj_t* container, bool has_border);

// Label styling function
void ui_theme_apply_label_style(lv_obj_t* label, bool is_title);

#ifdef __cplusplus
}
#endif

#endif // WAVEX_UI_THEME_H
