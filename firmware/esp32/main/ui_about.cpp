#include "ui_about.h"
#include "version.h"
#include <stdio.h>
#include <string.h>

// License text from our LICENSES.md file
static const char LICENSE_TEXT[] = 
"WaveX uses the following third-party libraries:\n\n"

"ESP32 FRONTEND (Audio-Optimized):\n"
"• ESP-IDF Framework - Apache License 2.0\n"
"  Copyright 2015-2024 Espressif Systems\n"
"  (Core components only - WiFi/BT disabled)\n\n"
"• FreeRTOS Kernel - MIT License\n"
"  Copyright (C) 2017 Amazon.com, Inc.\n\n"
"• LVGL Graphics Library - MIT License\n"
"  Copyright (c) 2016 Gabor Kiss-Vamosi\n"
"  Includes: Barcode, Expat, GifDec, LodePNG,\n"
"  LZ4, QR Code, ThorVG, TinyTTF, TJPGD,\n"
"  TLSF, Printf libraries\n\n"
"• XPT2046 Touchscreen - MIT License\n"
"  Copyright (c) 2015 Paul Stoffregen\n\n"

"DAISY BACKEND:\n"
"• libDaisy - MIT License\n"
"  Copyright (c) 2019 Electrosmith\n\n"
"• DaisySP - MIT License\n"
"  Copyright (c) 2020 Electrosmith\n"
"  Includes Plaits, Soundpipe libraries\n\n"
"• ARM CMSIS - Apache License 2.0\n"
"  Copyright (c) 2009-2017 ARM Limited\n\n"
"• STM32H7xx HAL Driver - BSD 3-Clause\n"
"  Copyright 2017 STMicroelectronics\n\n"

"All licenses permit commercial use.\n"
"Full license details in docs/LICENSES.md\n\n"

"ACKNOWLEDGMENTS:\n"
"We thank all library authors and contributors\n"
"for their invaluable work that makes WaveX possible.";

// Event handler for license modal close button
static void license_modal_close_cb(lv_event_t* e)
{
    lv_obj_t* modal = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_delete(modal);
}

// Event handler for "Show License Info" button
static void show_license_cb(lv_event_t* e)
{
    lv_obj_t* parent = (lv_obj_t*)lv_event_get_user_data(e);
    ui_about_show_license_modal(parent);
}

lv_obj_t* ui_about_create(lv_obj_t* parent_menu)
{
    // Create the About page
    lv_obj_t* about_page = lv_menu_page_create(parent_menu, "About");
    lv_obj_set_style_pad_hor(about_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(parent_menu), 0), 0);
    
    // Create main section
    lv_obj_t* section = lv_menu_section_create(about_page);
    
    // Project title and description
    lv_obj_t* title_cont = lv_menu_cont_create(section);
    lv_obj_t* title_label = lv_label_create(title_cont);
    lv_label_set_text(title_label, WAVEX_PROJECT_NAME);
    // Set title style
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_label, LV_PCT(100));
    
    lv_obj_t* desc_cont = lv_menu_cont_create(section);
    lv_obj_t* desc_label = lv_label_create(desc_cont);
    lv_label_set_text(desc_label, WAVEX_PROJECT_DESCRIPTION);
    // Set description style
    lv_obj_set_style_text_color(desc_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(desc_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(desc_label, LV_PCT(100));
    
    // Add separator
    lv_menu_separator_create(about_page);
    
    // Firmware versions section
    section = lv_menu_section_create(about_page);
    
    lv_obj_t* versions_cont = lv_menu_cont_create(section);
    lv_obj_t* versions_label = lv_label_create(versions_cont);
    
    char version_text[512];
    ui_about_get_version_info(version_text, sizeof(version_text));
    lv_label_set_text(versions_label, version_text);
    lv_obj_set_width(versions_label, LV_PCT(100));
    
    // Compile info
    lv_obj_t* compile_cont = lv_menu_cont_create(section);
    lv_obj_t* compile_label = lv_label_create(compile_cont);
    
    char compile_text[256];
    snprintf(compile_text, sizeof(compile_text), 
             "Built: %s %s", WAVEX_COMPILE_DATE, WAVEX_COMPILE_TIME);
    lv_label_set_text(compile_label, compile_text);
    lv_obj_set_width(compile_label, LV_PCT(100));
    
    // Add separator
    lv_menu_separator_create(about_page);
    
    // License button section
    section = lv_menu_section_create(about_page);
    
    lv_obj_t* license_cont = lv_menu_cont_create(section);
    lv_obj_add_flag(license_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* license_icon = lv_label_create(license_cont);
    lv_label_set_text(license_icon, LV_SYMBOL_FILE);
    
    lv_obj_t* license_label = lv_label_create(license_cont);
    lv_label_set_text(license_label, "License Information");
    lv_obj_set_flex_grow(license_label, 1);
    
    lv_obj_t* license_arrow = lv_label_create(license_cont);
    lv_label_set_text(license_arrow, LV_SYMBOL_RIGHT);
    
    // Add click event to license button
    lv_obj_add_event_cb(license_cont, show_license_cb, LV_EVENT_CLICKED, lv_screen_active());
    
    return about_page;
}

void ui_about_show_license_modal(lv_obj_t* parent)
{
    // Create modal backdrop
    lv_obj_t* modal = lv_obj_create(parent);
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(modal, 20, 0);
    
    // Create content container
    lv_obj_t* content = lv_obj_create(modal);
    lv_obj_set_size(content, LV_PCT(90), LV_PCT(85));
    lv_obj_center(content);
    lv_obj_set_style_radius(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    
    // Create header with title and close button
    lv_obj_t* header = lv_obj_create(content);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(header, 10, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "License Information");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    
    lv_obj_t* close_btn = lv_button_create(header);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_t* close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close_btn, license_modal_close_cb, LV_EVENT_CLICKED, modal);
    
    // Create scrollable text area
    lv_obj_t* text_area = lv_textarea_create(content);
    lv_obj_set_flex_grow(text_area, 1);
    lv_obj_set_width(text_area, LV_PCT(100));
    lv_textarea_set_text(text_area, LICENSE_TEXT);
    lv_obj_add_flag(text_area, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(text_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(text_area, &lv_font_montserrat_14, 0);
}

void ui_about_get_version_info(char* buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size,
             "Frontend (ESP32-S3):\n"
             "Version: %s\n\n"
             "Backend (Daisy Seed):\n"
             "Version: %s\n",
             WAVEX_FRONTEND_VERSION_STRING,
             WAVEX_BACKEND_VERSION_STRING);
} 