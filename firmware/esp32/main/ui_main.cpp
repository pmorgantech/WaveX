#include "ui_main.h"
#include "ui_about.h"
#include "version.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include <stdio.h>

static lv_obj_t* main_screen = NULL;
static lv_obj_t* main_menu = NULL;

// Helper function to create menu text items
static lv_obj_t* create_menu_text(lv_obj_t* parent, const char* icon, const char* txt)
{
    lv_obj_t* obj = lv_menu_cont_create(parent);

    if(icon) {
        lv_obj_t* img = lv_label_create(obj);
        lv_label_set_text(img, icon);
    }

    if(txt) {
        lv_obj_t* label = lv_label_create(obj);
        lv_label_set_text(label, txt);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(label, 1);
    }

    return obj;
}

// Helper function to create menu sliders (placeholder for future settings)
static lv_obj_t* create_menu_slider(lv_obj_t* parent, const char* icon, const char* txt, 
                                   int32_t min, int32_t max, int32_t val)
{
    lv_obj_t* obj = create_menu_text(parent, icon, txt);

    lv_obj_t* slider = lv_slider_create(obj);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, val, LV_ANIM_OFF);

    if(icon == NULL) {
        lv_obj_add_flag(slider, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    }

    return obj;
}

// Helper function to create menu switches (placeholder for future settings)
static lv_obj_t* create_menu_switch(lv_obj_t* parent, const char* icon, const char* txt, bool checked)
{
    lv_obj_t* obj = create_menu_text(parent, icon, txt);

    lv_obj_t* sw = lv_switch_create(obj);
    lv_obj_add_state(sw, checked ? LV_STATE_CHECKED : 0);

    return obj;
}

void ui_main_init(void)
{
    ESP_LOGI("UI", "Initializing esp_lvgl_port...");
    
    // Initialize esp_lvgl_port with display and touch
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    
    // TODO: Add display configuration here
    // - Configure ST7796S display driver
    // - Configure XPT2046 touch controller  
    // - Add display and touch to lvgl_port
    
    ESP_LOGI("UI", "Creating main UI elements...");
    
    // Lock LVGL for thread safety
    lvgl_port_lock(0);
    
    // Create main screen
    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x003a57), LV_PART_MAIN);
    lv_obj_set_style_text_color(main_screen, lv_color_hex(0xffffff), LV_PART_MAIN);
    
    // Create the main menu
    main_menu = ui_main_create_menu();
    
    // Load the main screen
    lv_screen_load(main_screen);
    
    // Unlock LVGL
    lvgl_port_unlock();
    
    ESP_LOGI("UI", "UI initialization complete");
}

lv_obj_t* ui_main_create_menu(void)
{
    // Create menu object
    lv_obj_t* menu = lv_menu_create(main_screen);
    
    // Style the menu background
    lv_color_t bg_color = lv_obj_get_style_bg_color(menu, 0);
    if(lv_color_brightness(bg_color) > 127) {
        lv_obj_set_style_bg_color(menu, lv_color_darken(lv_obj_get_style_bg_color(menu, 0), 10), 0);
    } else {
        lv_obj_set_style_bg_color(menu, lv_color_darken(lv_obj_get_style_bg_color(menu, 0), 50), 0);
    }
    
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_ENABLED);
    lv_obj_set_size(menu, lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
    lv_obj_center(menu);

    // Create sub-pages
    
    // Audio Settings page
    lv_obj_t* audio_page = lv_menu_page_create(menu, "Audio Settings");
    lv_obj_set_style_pad_hor(audio_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(audio_page);
    lv_obj_t* section = lv_menu_section_create(audio_page);
    create_menu_slider(section, LV_SYMBOL_VOLUME_MAX, "Master Volume", 0, 100, 75);
    create_menu_slider(section, LV_SYMBOL_AUDIO, "Sample Rate", 44100, 96000, 48000);
    create_menu_switch(section, LV_SYMBOL_MUTE, "Mute", false);

    // Display Settings page
    lv_obj_t* display_page = lv_menu_page_create(menu, "Display Settings");
    lv_obj_set_style_pad_hor(display_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(display_page);
    section = lv_menu_section_create(display_page);
    create_menu_slider(section, LV_SYMBOL_EYE_OPEN, "Brightness", 0, 100, 80);
    create_menu_switch(section, LV_SYMBOL_REFRESH, "Auto Sleep", true);

    // System Settings page with About
    lv_obj_t* system_page = lv_menu_page_create(menu, "System");
    lv_obj_set_style_pad_hor(system_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(system_page);
    section = lv_menu_section_create(system_page);
    
    // System info items
    create_menu_switch(section, LV_SYMBOL_USB, "USB Mode", false);
    create_menu_text(section, LV_SYMBOL_SD_CARD, "SD Card: Ready");
    create_menu_text(section, LV_SYMBOL_POWER, "Power: Optimized");
    
    // About subsection
    lv_menu_separator_create(system_page);
    section = lv_menu_section_create(system_page);
    
    // Create About page and link it
    lv_obj_t* about_page = ui_about_create(menu);
    lv_obj_t* about_item = create_menu_text(section, LV_SYMBOL_HOME, "About");
    lv_menu_set_load_page_event(menu, about_item, about_page);

    // Create root page
    lv_obj_t* root_page = lv_menu_page_create(menu, "WaveX");
    lv_obj_set_style_pad_hor(root_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    
    // Main sections
    section = lv_menu_section_create(root_page);
    lv_obj_t* audio_item = create_menu_text(section, LV_SYMBOL_AUDIO, "Audio");
    lv_menu_set_load_page_event(menu, audio_item, audio_page);
    
    lv_obj_t* display_item = create_menu_text(section, LV_SYMBOL_EYE_OPEN, "Display");
    lv_menu_set_load_page_event(menu, display_item, display_page);
    
    lv_obj_t* system_item = create_menu_text(section, LV_SYMBOL_SETTINGS, "System");
    lv_menu_set_load_page_event(menu, system_item, system_page);

    // Performance section
    lv_obj_t* perf_label = lv_label_create(root_page);
    lv_label_set_text(perf_label, "Performance");
    lv_obj_set_style_text_font(perf_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(perf_label, 10, 0);
    section = lv_menu_section_create(root_page);
    
    // Add some performance info (audio-optimized)
    char cpu_info[64];
    snprintf(cpu_info, sizeof(cpu_info), "CPU: ESP32-S3 @ 240MHz (Audio Mode)");
    create_menu_text(section, LV_SYMBOL_CHARGE, cpu_info);
    
    char memory_info[64];
    snprintf(memory_info, sizeof(memory_info), "RAM: 512KB SRAM + PSRAM");
    create_menu_text(section, LV_SYMBOL_LIST, memory_info);
    
    char wireless_info[64];
    snprintf(wireless_info, sizeof(wireless_info), "Wireless: Disabled (Power Saving)");
    create_menu_text(section, LV_SYMBOL_POWER, wireless_info);

    // Set the sidebar to show the root page
    lv_menu_set_sidebar_page(menu, root_page);

    // Start by showing the first item
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(lv_menu_get_cur_sidebar_page(menu), 0), 0), 
                     LV_EVENT_CLICKED, NULL);

    return menu;
}

lv_obj_t* ui_main_get_screen(void)
{
    return main_screen;
} 