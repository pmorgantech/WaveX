#ifndef LVGL_H
#define LVGL_H

// Mock LVGL header for unit testing
// Provides minimal types and functions needed by file_browser.cpp

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL object types
typedef void lv_obj_t;
typedef int32_t lv_coord_t;
typedef struct {
    uint8_t r, g, b;
} lv_color_t;
typedef int lv_style_selector_t;
typedef int lv_align_t;
typedef struct lv_event_t {
    void* target;
    void* current_target;
    void* user_data;
    int code;
} lv_event_t;
typedef void (*lv_event_cb_t)(lv_event_t*);
typedef int lv_event_code_t;
typedef struct {
} lv_font_t;
extern const lv_font_t lv_font_montserrat_18;
extern const lv_font_t lv_font_montserrat_26;
extern const lv_font_t lv_font_montserrat_36;

// LVGL constants
#define LV_ALIGN_TOP_LEFT 0
#define LV_ALIGN_TOP_MID 1
#define LV_ALIGN_TOP_RIGHT 2
#define LV_PART_MAIN 0
#define LV_EVENT_CLICKED 0

// LVGL functions (implemented in test file)
lv_obj_t* lv_obj_create(lv_obj_t* parent);
void lv_obj_set_size(lv_obj_t* obj, lv_coord_t w, lv_coord_t h);
void lv_obj_set_style_bg_color(lv_obj_t* obj, lv_color_t color, lv_style_selector_t selector);
void lv_obj_set_style_border_width(lv_obj_t* obj, int32_t width, lv_style_selector_t selector);
void lv_obj_set_style_pad_all(lv_obj_t* obj, int32_t pad, lv_style_selector_t selector);
void lv_obj_align(lv_obj_t* obj, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs);
lv_obj_t* lv_label_create(lv_obj_t* parent);
void lv_label_set_text(lv_obj_t* label, const char* text);
lv_obj_t* lv_list_create(lv_obj_t* parent);
void lv_obj_add_event_cb(lv_obj_t* obj,
                         lv_event_cb_t event_cb,
                         lv_event_code_t filter,
                         void* user_data);
void lv_obj_del(lv_obj_t* obj);
lv_coord_t lv_pct(int32_t value);
lv_obj_t* lv_event_get_current_target(lv_event_t* e);
lv_obj_t* lv_event_get_target(lv_event_t* e);
lv_event_code_t lv_event_get_code(lv_event_t* e);
void* lv_event_get_user_data(lv_event_t* e);
lv_obj_t* lv_obj_get_child(lv_obj_t* obj, int32_t id);
int32_t lv_obj_get_child_cnt(lv_obj_t* obj);
void lv_obj_set_style_text_color(lv_obj_t* obj, lv_color_t color, lv_style_selector_t selector);
lv_obj_t* lv_list_add_btn(lv_obj_t* list, const void* img, const char* txt);
void lv_obj_set_style_text_font(lv_obj_t* obj, const void* font, lv_style_selector_t selector);
void lv_obj_set_style_border_color(lv_obj_t* obj, lv_color_t color, lv_style_selector_t selector);
lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b);
void lv_obj_clean(lv_obj_t* obj);
void lv_obj_set_user_data(lv_obj_t* obj, void* user_data);
void* lv_obj_get_user_data(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif

#endif  // LVGL_H
