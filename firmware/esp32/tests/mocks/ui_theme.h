#ifndef UI_THEME_H
#define UI_THEME_H

// Mock UI theme header for unit testing

#include "lvgl.h"

// UI theme constants
#define UI_COLOR_CONTENT {0, 0, 0}
#define UI_COLOR_TEXT {255, 255, 255}
#define UI_PADDING_SMALL 4
#define UI_PADDING_MEDIUM 8

// Font references (pointers to LVGL fonts)
#define UI_FONT_NORMAL &lv_font_montserrat_18
#define UI_FONT_TITLE &lv_font_montserrat_26
#define UI_FONT_HEADER &lv_font_montserrat_36

// UI theme functions (C++ linkage - bool is C++ type)
#ifdef __cplusplus
void ui_theme_apply_label_style(lv_obj_t* label, bool is_header);
void ui_theme_apply_button_style(lv_obj_t* btn, bool is_selected);
#else
// C compatibility (shouldn't be used in C code, but provide for completeness)
void ui_theme_apply_label_style(lv_obj_t* label, int is_header);
void ui_theme_apply_button_style(lv_obj_t* btn, int is_selected);
#endif

#endif  // UI_THEME_H
