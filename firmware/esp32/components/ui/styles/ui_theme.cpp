/**
 * @file ui_theme.cpp
 * @brief UI Theme and Styling Implementation
 */

#include "ui_theme.h"
#include "esp_log.h"

static const char *TAG = "UI_THEME";

void ui_theme_apply_button_style(lv_obj_t* button, bool is_pressed_style)
{
    if (!button) return;
    
    // Background colors
    lv_obj_set_style_bg_color(button, UI_COLOR_BUTTON, LV_PART_MAIN);
    if (is_pressed_style) {
        lv_obj_set_style_bg_color(button, UI_COLOR_BUTTON_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    }
    
    // Border styling
    lv_obj_set_style_border_width(button, UI_BORDER_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, UI_COLOR_BUTTON_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, UI_BORDER_RADIUS, LV_PART_MAIN);
    
    // Ensure button is clickable
    lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
}

void ui_theme_apply_container_style(lv_obj_t* container, bool has_border)
{
    if (!container) return;
    
    // Background color
    lv_obj_set_style_bg_color(container, UI_COLOR_CONTENT, LV_PART_MAIN);
    
    // Border styling
    if (has_border) {
        lv_obj_set_style_border_width(container, UI_BORDER_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_border_color(container, UI_COLOR_BORDER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    }
    
    // Padding
    lv_obj_set_style_pad_all(container, UI_PADDING_MEDIUM, LV_PART_MAIN);
}

void ui_theme_apply_label_style(lv_obj_t* label, bool is_title)
{
    if (!label) return;
    
    // Text color
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, LV_PART_MAIN);
    
    // Font selection
    if (is_title) {
        lv_obj_set_style_text_font(label, UI_FONT_TITLE, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_font(label, UI_FONT_NORMAL, LV_PART_MAIN);
    }
}
