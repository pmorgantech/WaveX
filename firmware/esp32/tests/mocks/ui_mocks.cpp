// Mock implementations for UI components unit tests
// These provide minimal stubs for LVGL and UI theme functions

#include "lvgl.h"
#include "ui_theme.h"

#include <cstddef>  // For size_t

// Mock LVGL objects
static void* mock_lvgl_objects[10] = {nullptr};
static int mock_lvgl_obj_count = 0;

// Mock LVGL font objects
const lv_font_t lv_font_montserrat_18 = {};
const lv_font_t lv_font_montserrat_26 = {};
const lv_font_t lv_font_montserrat_36 = {};

extern "C" {

lv_obj_t* lv_obj_create(lv_obj_t* parent) {
    (void)parent;
    return (lv_obj_t*)(&mock_lvgl_objects[mock_lvgl_obj_count++ % 10]);
}

void lv_obj_set_size(lv_obj_t* obj, lv_coord_t w, lv_coord_t h) {
    (void)obj;
    (void)w;
    (void)h;
}
void lv_obj_set_style_bg_color(lv_obj_t* obj, lv_color_t color, lv_style_selector_t selector) {
    (void)obj;
    (void)color;
    (void)selector;
}
void lv_obj_set_style_border_width(lv_obj_t* obj, int32_t width, lv_style_selector_t selector) {
    (void)obj;
    (void)width;
    (void)selector;
}
void lv_obj_set_style_pad_all(lv_obj_t* obj, int32_t pad, lv_style_selector_t selector) {
    (void)obj;
    (void)pad;
    (void)selector;
}
void lv_obj_align(lv_obj_t* obj, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs) {
    (void)obj;
    (void)align;
    (void)x_ofs;
    (void)y_ofs;
}
lv_obj_t* lv_label_create(lv_obj_t* parent) {
    return lv_obj_create(parent);
}
void lv_label_set_text(lv_obj_t* label, const char* text) {
    (void)label;
    (void)text;
}
lv_obj_t* lv_list_create(lv_obj_t* parent) {
    return lv_obj_create(parent);
}
void lv_obj_add_event_cb(lv_obj_t* obj,
                         lv_event_cb_t event_cb,
                         lv_event_code_t filter,
                         void* user_data) {
    (void)obj;
    (void)event_cb;
    (void)filter;
    (void)user_data;
}
void lv_obj_del(lv_obj_t* obj) {
    (void)obj;
}
lv_coord_t lv_pct(int32_t value) {
    (void)value;
    return 0;
}
lv_obj_t* lv_event_get_current_target(lv_event_t* e) {
    return e ? (lv_obj_t*)e->current_target : nullptr;
}
lv_obj_t* lv_event_get_target(lv_event_t* e) {
    return e ? (lv_obj_t*)e->target : nullptr;
}
lv_event_code_t lv_event_get_code(lv_event_t* e) {
    return e ? (lv_event_code_t)e->code : 0;
}
void* lv_event_get_user_data(lv_event_t* e) {
    return e ? e->user_data : nullptr;
}
lv_obj_t* lv_obj_get_child(lv_obj_t* obj, int32_t id) {
    (void)obj;
    (void)id;
    return nullptr;
}
int32_t lv_obj_get_child_cnt(lv_obj_t* obj) {
    (void)obj;
    return 0;
}
void lv_obj_set_style_text_color(lv_obj_t* obj, lv_color_t color, lv_style_selector_t selector) {
    (void)obj;
    (void)color;
    (void)selector;
}
lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b) {
    lv_color_t c = {r, g, b};
    return c;
}
lv_obj_t* lv_list_add_btn(lv_obj_t* list, const void* img, const char* txt) {
    (void)img;
    (void)txt;
    return lv_obj_create(list);
}
void lv_obj_set_style_text_font(lv_obj_t* obj, const void* font, lv_style_selector_t selector) {
    (void)obj;
    (void)font;
    (void)selector;
}
void lv_obj_set_style_border_color(lv_obj_t* obj, lv_color_t color, lv_style_selector_t selector) {
    (void)obj;
    (void)color;
    (void)selector;
}
void lv_obj_clean(lv_obj_t* obj) {
    (void)obj;
}
void lv_obj_set_user_data(lv_obj_t* obj, void* user_data) {
    (void)obj;
    (void)user_data;
}
void* lv_obj_get_user_data(lv_obj_t* obj) {
    (void)obj;
    return nullptr;
}

}  // extern "C" - end LVGL functions

// Mock UI theme functions (C++ linkage due to bool parameters)
#include "esp_err.h"
void ui_theme_apply_label_style(lv_obj_t* label, bool is_header) {
    (void)label;
    (void)is_header;
}
void ui_theme_apply_button_style(lv_obj_t* btn, bool is_selected) {
    (void)btn;
    (void)is_selected;
}

// Mock inter-MCU functions (C++ linkage - declared without extern "C" in inter_mcu.h)
typedef void (*wavex_browse_resp_cb_t)(const uint8_t* data, size_t length, void* user_data);
void inter_mcu_set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data) {
    (void)cb;
    (void)user_data;
}
esp_err_t inter_mcu_send_browse_req(const char* path, uint8_t start_index) {
    (void)path;
    (void)start_index;
    return ESP_OK;  // Return success for testing
}
