/**
 * @file ui_task.cpp
 * @brief UI Task Implementation for MIPI DSI Display with LVGL
 * 
 * This implementation provides full LVGL integration with MIPI DSI display
 * using the Waveshare 5-DSI-TOUCH-A display and HX8394 driver.
 */

#include "../components/ui/pages/sample_load_save.h"
#include "ui_task.h"
#include "pcnt_task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portable.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include <stdlib.h>
#include "driver/i2c_master.h"
#include "hardware_pins.h"
#include "pages/diagnostics_page.h"
#include "inter_mcu.h"
#include "comm/statistics.h"
#include "links/esp_spi_link.h"
 
// LVGL includes
#include "esp_lvgl_port.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_io_i2c.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_demo.h"

// LVGL port lock macros for thread safety
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

// Include BSP header for display functions
#include "bsp/esp32_p4_nano.h"
#include "ui/tca8418_keypad.h"
#include "pin_config.h"
 
#define LV_TICK_PERIOD_MS 5
 
 static const char *TAG = "UI_TASK";
 
 // Task handle
 static TaskHandle_t s_ui_task_handle = NULL;
 
// Display and LVGL handles
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_lvgl_display = NULL;
static esp_timer_handle_t s_lvgl_tick_timer_handle = NULL;
static esp_timer_handle_t s_meter_timer_handle = NULL;
 
// I2C handles
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

// UI element references for real-time updates
static lv_obj_t *s_content_area = NULL;

// Hotkey region elements
static lv_obj_t *s_hotkey_region = NULL;
static lv_obj_t *s_hotkey_buttons[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
static lv_obj_t *s_hotkey_labels[6] = {NULL, NULL, NULL, NULL, NULL, NULL};

// Header elements
static lv_obj_t *s_header_title = NULL;

// Current screen context for hotkey mapping
static const char* s_current_screen = "main";

// Sample Load/Save page instance (global for access from other modules)
wavex_sample_load_save_page_t* s_sample_load_save_page = NULL;

// Meter callback system
static wavex_meter_cb_t s_meter_callback = NULL;
static void* s_meter_user_data = NULL;

// Meter data variables
static float s_current_rms = 0.0f;
static float s_current_peak = 0.0f;
static lv_obj_t *s_meter_bar = NULL;
static lv_obj_t *s_meter_label = NULL;

// Enhanced meter display variables
static lv_obj_t *s_meter_bar_l = NULL;
static lv_obj_t *s_meter_bar_r = NULL;
static lv_obj_t *s_peak_line_l = NULL;
static lv_obj_t *s_peak_line_r = NULL;
static lv_obj_t *s_meter_label_l = NULL;
static lv_obj_t *s_meter_label_r = NULL;

// Peak hold tracking
struct PeakHoldData {
    float peak_value;
    uint32_t peak_time_ms;
    bool is_holding;
};
static PeakHoldData s_peak_hold_l = {0.0f, 0, false};
static PeakHoldData s_peak_hold_r = {0.0f, 0, false};
static const uint32_t PEAK_HOLD_TIMEOUT_MS = 500;  // Configurable peak hold duration

// Adaptive refresh rate control
static bool s_content_changed = false;
static uint32_t s_last_refresh_time = 0;
static uint32_t s_refresh_count = 0;
static const uint32_t MIN_REFRESH_INTERVAL_MS = 16;  // 60 FPS maximum
static const uint32_t MAX_REFRESH_INTERVAL_MS = 100; // 10 FPS minimum

// Deferred meter update data
static bool s_meter_update_pending = false;
static bool s_meter_reset_pending = false;
static float s_deferred_rms_left = 0.0f;
static float s_deferred_rms_right = 0.0f;
static float s_deferred_peak_left = 0.0f;
static float s_deferred_peak_right = 0.0f;
 
// Forward declarations
static void ui_task(void *pvParameters);
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static void lvgl_tick_cb(void *arg);
static void meter_update_cb(void *arg);
static void meter_data_cb(float rms, float peak, void* user_data);
static esp_err_t init_touch_controller(void);
static void adaptive_refresh_control(void);
void wavex_ui_mark_content_changed(void);

// Enhanced meter functions
static void update_peak_hold(PeakHoldData* peak_data, float current_peak, uint32_t current_time_ms);
static void update_peak_line_position(lv_obj_t* peak_line, lv_obj_t* meter_bar, float peak_value);
void wavex_ui_create_meter_display(lv_obj_t* parent);
static esp_err_t init_lvgl_display(void);

// Menu system functions
static void create_main_menu(lv_obj_t *parent);
static void create_sample_menu(lv_obj_t *parent);
static void create_system_menu(lv_obj_t *parent);
static void create_sample_load_save_page(lv_obj_t *parent);
static void menu_button_event_cb(lv_event_t *e);
static void hotkey_button_event_cb(lv_event_t *e);
static void touch_event_cb(lv_event_t *e);

// Hotkey region functions
static void create_hotkey_region(lv_obj_t *parent);
void wavex_ui_update_hotkey_labels(const char* labels[6]);
static void clear_hotkey_labels(void);
static void calculate_hotkey_layout(int num_items, int* button_widths);
void wavex_ui_set_screen_context(const char* screen_name);
void wavex_ui_update_header_title(const char* title);
 
/**
 * @brief Initialize touch controller with custom configuration
 */
static esp_err_t init_touch_controller(void)
{
    ESP_LOGI(TAG, "Initializing GT911 touch controller with custom config...");

    // Use BSP's I2C handle to avoid conflicts
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    if (i2c_handle == NULL) {
        ESP_LOGE(TAG, "BSP I2C not initialized, initializing it first");
        ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Failed to initialize BSP I2C");
        i2c_handle = bsp_i2c_get_handle();
    }

    // Configure touch reset and interrupt pins for GT911 address selection
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WAVEX_ESP_TOUCH_RST) | (1ULL << WAVEX_ESP_TOUCH_INT),  // RST and INT pins
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure touch GPIO pins");

    // --- GT911 Reset and Address Selection Sequence ---
    // Per datasheet, this selects the I2C address. We will select 0x5D.
    gpio_set_level(WAVEX_ESP_TOUCH_INT, 1);  // INT high for address 0x5D
    gpio_set_level(WAVEX_ESP_TOUCH_RST, 0);  // RST low
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(WAVEX_ESP_TOUCH_RST, 1);  // RST high
    vTaskDelay(pdMS_TO_TICKS(60)); // Hold INT state for >50ms after reset

    // Release INT pin to be an input for interrupts
    io_conf.pin_bit_mask = (1ULL << WAVEX_ESP_TOUCH_INT);  // Only INT pin
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE; // INT line usually requires a pull-up
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to reconfigure INT pin to input");

    vTaskDelay(pdMS_TO_TICKS(10)); // Settle time
    // --- End of GT911 Sequence ---

    // Create I2C panel IO for touch controller using BSP's I2C handle
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, // Use 0x5D address
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
        .scl_speed_hz = 400000,  // 400kHz for GT911
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v2(i2c_handle, &io_config, &io_handle),
                       TAG, "Failed to create I2C panel IO");

    // Configure GT911 touch controller with correct orientation for 720x1280 display
    esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = WAVEX_ESP_TOUCH_RST,  // Touch reset pin
        .int_gpio_num = WAVEX_ESP_TOUCH_INT,  // Touch interrupt pin
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,     // No swap for portrait orientation
            .mirror_x = 0,    // No mirror X
            .mirror_y = 0,    // No mirror Y
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io_handle, &touch_config, &s_touch_handle),
                       TAG, "Failed to create GT911 touch controller");

    ESP_LOGI(TAG, "GT911 touch controller initialized successfully");
    return ESP_OK;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) lv_display_get_user_data(disp);
    // LVGL handles rotation internally, coordinates are already transformed
    // For 90° clockwise rotation: area coordinates should be correct as provided
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}
 
static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

/**
 * @brief Update peak hold tracking for a channel
 */
static void update_peak_hold(PeakHoldData* peak_data, float current_peak, uint32_t current_time_ms)
{
    if (current_peak > peak_data->peak_value) {
        // New peak detected - start holding
        peak_data->peak_value = current_peak;
        peak_data->peak_time_ms = current_time_ms;
        peak_data->is_holding = true;
    } else if (peak_data->is_holding && 
               (current_time_ms - peak_data->peak_time_ms) >= PEAK_HOLD_TIMEOUT_MS) {
        // Peak hold timeout expired - revert to current peak
        peak_data->peak_value = current_peak;
        peak_data->is_holding = false;
    }
}

/**
 * @brief Update peak line position on meter bar
 */
static void update_peak_line_position(lv_obj_t* peak_line, lv_obj_t* meter_bar, float peak_value)
{
    if (!peak_line || !meter_bar || !lv_obj_is_valid(peak_line) || !lv_obj_is_valid(meter_bar)) {
        return;
    }
    
    // Get meter bar dimensions (peak line is now a child of meter bar)
    lv_coord_t bar_width = lv_obj_get_width(meter_bar);
    lv_coord_t bar_height = lv_obj_get_height(meter_bar);
    
    // Calculate peak position (0-100% of bar width)
    int peak_percent = (int)(peak_value * 100.0f);
    if (peak_percent > 100) peak_percent = 100;
    if (peak_percent < 0) peak_percent = 0;
    
    // Position peak line within the meter bar (relative to bar)
    lv_coord_t peak_x = (bar_width * peak_percent) / 100;
    lv_obj_set_pos(peak_line, peak_x, 0);
    lv_obj_set_size(peak_line, 2, bar_height);  // 2px wide vertical line
    
    // Show/hide peak line based on peak value
    if (peak_value > 0.01f) {  // Show if peak > 1%
        lv_obj_clear_flag(peak_line, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(peak_line, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Meter update timer callback for real-time updates
 */
static void meter_update_cb(void *arg)
{
    // Get current meter data from statistics
    wavex_meter_data_t meter_data;
    inter_mcu_get_meter_data(&meter_data);
    
    uint32_t current_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool should_reset_meters = false;
    
    // Check for communication timeout (500ms)
    if (meter_data.valid) {
        uint32_t time_since_update = current_time_ms - meter_data.last_update_ms;
        if (time_since_update > 500) {
            should_reset_meters = true;
        }
    } else {
        // No valid data received, reset meters (normal when no audio is running)
        should_reset_meters = true;
    }
    
    if (should_reset_meters) {
        // Only reset if we haven't already reset recently (avoid continuous updates)
        static uint32_t last_reset_time = 0;
        uint32_t time_since_reset = current_time_ms - last_reset_time;
        
        if (time_since_reset > 1000) { // Only reset once per second
            // Reset peak hold values to 0
            s_peak_hold_l.peak_value = 0.0f;
            s_peak_hold_l.peak_time_ms = current_time_ms;
            s_peak_hold_l.is_holding = false;
            
            s_peak_hold_r.peak_value = 0.0f;
            s_peak_hold_r.peak_time_ms = current_time_ms;
            s_peak_hold_r.is_holding = false;
            
            last_reset_time = current_time_ms;
            
            // Mark meter reset as pending (will be processed by main UI task)
            s_meter_reset_pending = true;
            wavex_ui_mark_content_changed();
        }
    } else if (meter_data.valid) {
        // Normal operation - update meters with valid data
        // Update peak hold tracking
        update_peak_hold(&s_peak_hold_l, meter_data.peak_left, current_time_ms);
        update_peak_hold(&s_peak_hold_r, meter_data.peak_right, current_time_ms);
        
        // Store meter data for deferred update (no LVGL locks in timer callback)
        s_deferred_rms_left = meter_data.rms_left;
        s_deferred_rms_right = meter_data.rms_right;
        s_deferred_peak_left = s_peak_hold_l.peak_value;
        s_deferred_peak_right = s_peak_hold_r.peak_value;
        
        // Mark meter update as pending (will be processed by main UI task)
        s_meter_update_pending = true;
        wavex_ui_mark_content_changed();
    }
}

/**
 * @brief Process deferred meter updates (called from main UI task)
 */
static void process_deferred_meter_updates(void)
{
    if (s_meter_reset_pending) {
        LV_LOCK();
        
        // Reset enhanced meter display if available
        if (s_meter_bar_l && s_meter_bar_r && s_peak_line_l && s_peak_line_r &&
            lv_obj_is_valid(s_meter_bar_l) && lv_obj_is_valid(s_meter_bar_r) &&
            lv_obj_is_valid(s_peak_line_l) && lv_obj_is_valid(s_peak_line_r)) {
            
            // Reset RMS bars to 0
            lv_bar_set_value(s_meter_bar_l, 0, LV_ANIM_ON);
            lv_bar_set_value(s_meter_bar_r, 0, LV_ANIM_ON);
            
            // Hide peak lines (they will be hidden by update_peak_line_position with 0 value)
            update_peak_line_position(s_peak_line_l, s_meter_bar_l, 0.0f);
            update_peak_line_position(s_peak_line_r, s_meter_bar_r, 0.0f);
            
            // Update channel labels to show 0 values
            if (s_meter_label_l && lv_obj_is_valid(s_meter_label_l)) {
                lv_label_set_text(s_meter_label_l, "L: 0.000\nPeak: 0.000");
            }
            
            if (s_meter_label_r && lv_obj_is_valid(s_meter_label_r)) {
                lv_label_set_text(s_meter_label_r, "R: 0.000\nPeak: 0.000");
            }
        }
        
        // Reset fallback legacy single meter display
        else if (s_meter_bar && s_meter_label && 
                 lv_obj_is_valid(s_meter_bar) && lv_obj_is_valid(s_meter_label)) {
            lv_bar_set_value(s_meter_bar, 0, LV_ANIM_ON);
            lv_label_set_text(s_meter_label, "L: 0.000 R: 0.000\nPeak L: 0.000 R: 0.000");
        }
        
        LV_UNLOCK();
        s_meter_reset_pending = false;
    }
    
    if (s_meter_update_pending) {
        LV_LOCK();
        
        // Update enhanced meter display if available
        if (s_meter_bar_l && s_meter_bar_r && s_peak_line_l && s_peak_line_r &&
            lv_obj_is_valid(s_meter_bar_l) && lv_obj_is_valid(s_meter_bar_r) &&
            lv_obj_is_valid(s_peak_line_l) && lv_obj_is_valid(s_peak_line_r)) {
            
            // Update left channel RMS bar
            int rms_l_value = (int)(s_deferred_rms_left * 100.0f);
            if (rms_l_value > 100) rms_l_value = 100;
            if (rms_l_value < 0) rms_l_value = 0;
            lv_bar_set_value(s_meter_bar_l, rms_l_value, LV_ANIM_ON);
            
            // Update right channel RMS bar
            int rms_r_value = (int)(s_deferred_rms_right * 100.0f);
            if (rms_r_value > 100) rms_r_value = 100;
            if (rms_r_value < 0) rms_r_value = 0;
            lv_bar_set_value(s_meter_bar_r, rms_r_value, LV_ANIM_ON);
            
            // Update peak line positions
            update_peak_line_position(s_peak_line_l, s_meter_bar_l, s_deferred_peak_left);
            update_peak_line_position(s_peak_line_r, s_meter_bar_r, s_deferred_peak_right);
            
            // Update channel labels
            if (s_meter_label_l && lv_obj_is_valid(s_meter_label_l)) {
                char label_text[32];
                snprintf(label_text, sizeof(label_text), "L: %.3f\nPeak: %.3f", 
                        s_deferred_rms_left, s_deferred_peak_left);
                lv_label_set_text(s_meter_label_l, label_text);
            }
            
            if (s_meter_label_r && lv_obj_is_valid(s_meter_label_r)) {
                char label_text[32];
                snprintf(label_text, sizeof(label_text), "R: %.3f\nPeak: %.3f", 
                        s_deferred_rms_right, s_deferred_peak_right);
                lv_label_set_text(s_meter_label_r, label_text);
            }
        }
        
        // Fallback to legacy single meter display
        else if (s_meter_bar && s_meter_label && 
                 lv_obj_is_valid(s_meter_bar) && lv_obj_is_valid(s_meter_label)) {
            // Use RMS left channel for the bar (assuming stereo)
            float rms_value = s_deferred_rms_left;
            int bar_value = (int)(rms_value * 100.0f);
            if (bar_value > 100) bar_value = 100;
            if (bar_value < 0) bar_value = 0;
            
            lv_bar_set_value(s_meter_bar, bar_value, LV_ANIM_ON);
            
            // Update label with current values
            char meter_text[64];
            snprintf(meter_text, sizeof(meter_text), "L: %.3f R: %.3f\nPeak L: %.3f R: %.3f", 
                    s_deferred_rms_left, s_deferred_rms_right, 
                    s_deferred_peak_left, s_deferred_peak_right);
            lv_label_set_text(s_meter_label, meter_text);
        }
        
        LV_UNLOCK();
        s_meter_update_pending = false;
    }
}

/**
 * @brief Create enhanced dual-channel meter display with peak hold
 */
void wavex_ui_create_meter_display(lv_obj_t* parent)
{
    // Create meter container - full width and height of parent for 3-column layout
    lv_obj_t *meter_area = lv_obj_create(parent);
    lv_obj_set_size(meter_area, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(meter_area, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN); // Dark gray background to match columns
    lv_obj_set_style_border_width(meter_area, 0, LV_PART_MAIN); // No border since parent column has border
    lv_obj_align(meter_area, LV_ALIGN_TOP_LEFT, 0, 0);

    // Meter title
    lv_obj_t *meter_title = lv_label_create(meter_area);
    lv_label_set_text(meter_title, "Audio Meters");
    lv_obj_set_style_text_color(meter_title, lv_color_white(), LV_PART_MAIN); // White text for dark mode
    lv_obj_set_style_text_font(meter_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(meter_title, LV_ALIGN_TOP_MID, 0, 5);

    // Left channel container
    lv_obj_t *left_channel = lv_obj_create(meter_area);
    lv_obj_set_size(left_channel, lv_pct(45), lv_pct(70));
    lv_obj_set_style_bg_color(left_channel, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN); // Dark gray background
    lv_obj_set_style_border_width(left_channel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(left_channel, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_MAIN);
    lv_obj_align(left_channel, LV_ALIGN_LEFT_MID, 0, 0);

    // Left channel label
    lv_obj_t *left_label = lv_label_create(left_channel);
    lv_label_set_text(left_label, "LEFT");
    lv_obj_set_style_text_color(left_label, lv_color_white(), LV_PART_MAIN); // White text for dark mode
    lv_obj_set_style_text_font(left_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(left_label, LV_ALIGN_TOP_MID, 0, 5);

    // Left channel RMS bar
    s_meter_bar_l = lv_bar_create(left_channel);
    lv_obj_set_size(s_meter_bar_l, lv_pct(80), 20);
    lv_obj_align(s_meter_bar_l, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(s_meter_bar_l, 0, 100);
    lv_bar_set_value(s_meter_bar_l, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_meter_bar_l, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_INDICATOR);

    // Left channel peak line (initially hidden) - as child of meter bar
    s_peak_line_l = lv_obj_create(s_meter_bar_l);
    lv_obj_set_size(s_peak_line_l, 2, 20);
    lv_obj_set_style_bg_color(s_peak_line_l, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_peak_line_l, 0, LV_PART_MAIN);
    lv_obj_set_pos(s_peak_line_l, 0, 0);
    lv_obj_add_flag(s_peak_line_l, LV_OBJ_FLAG_HIDDEN);

    // Left channel values label
    s_meter_label_l = lv_label_create(left_channel);
    lv_label_set_text(s_meter_label_l, "L: 0.000\nPeak: 0.000");
    lv_obj_set_style_text_color(s_meter_label_l, lv_color_white(), LV_PART_MAIN); // White text for dark mode
    lv_obj_set_style_text_font(s_meter_label_l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_meter_label_l, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Right channel container
    lv_obj_t *right_channel = lv_obj_create(meter_area);
    lv_obj_set_size(right_channel, lv_pct(45), lv_pct(70));
    lv_obj_set_style_bg_color(right_channel, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN); // Dark gray background
    lv_obj_set_style_border_width(right_channel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(right_channel, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_MAIN);
    lv_obj_align(right_channel, LV_ALIGN_RIGHT_MID, 0, 0);

    // Right channel label
    lv_obj_t *right_label = lv_label_create(right_channel);
    lv_label_set_text(right_label, "RIGHT");
    lv_obj_set_style_text_color(right_label, lv_color_white(), LV_PART_MAIN); // White text for dark mode
    lv_obj_set_style_text_font(right_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(right_label, LV_ALIGN_TOP_MID, 0, 5);

    // Right channel RMS bar
    s_meter_bar_r = lv_bar_create(right_channel);
    lv_obj_set_size(s_meter_bar_r, lv_pct(80), 20);
    lv_obj_align(s_meter_bar_r, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(s_meter_bar_r, 0, 100);
    lv_bar_set_value(s_meter_bar_r, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_meter_bar_r, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_INDICATOR);

    // Right channel peak line (initially hidden) - as child of meter bar
    s_peak_line_r = lv_obj_create(s_meter_bar_r);
    lv_obj_set_size(s_peak_line_r, 2, 20);
    lv_obj_set_style_bg_color(s_peak_line_r, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_peak_line_r, 0, LV_PART_MAIN);
    lv_obj_set_pos(s_peak_line_r, 0, 0);
    lv_obj_add_flag(s_peak_line_r, LV_OBJ_FLAG_HIDDEN);

    // Right channel values label
    s_meter_label_r = lv_label_create(right_channel);
    lv_label_set_text(s_meter_label_r, "R: 0.000\nPeak: 0.000");
    lv_obj_set_style_text_color(s_meter_label_r, lv_color_white(), LV_PART_MAIN); // White text for dark mode
    lv_obj_set_style_text_font(s_meter_label_r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_meter_label_r, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Peak hold info label
    lv_obj_t *peak_info = lv_label_create(meter_area);
    lv_label_set_text(peak_info, "Peak Hold: 500ms");
    lv_obj_set_style_text_color(peak_info, lv_color_white(), LV_PART_MAIN); // White text for dark mode
    lv_obj_set_style_text_font(peak_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(peak_info, LV_ALIGN_BOTTOM_MID, 0, -5);
}

/**
 * @brief Meter data callback from Daisy
 */
static void meter_data_cb(float rms, float peak, void* user_data)
{
    // Debug logging for meter data
    #ifdef WAVEX_LOG_METER_DATA
    ESP_LOGI(TAG, "Meter data received: RMS=%.3f, Peak=%.3f", rms, peak);
    #endif
    
    s_current_rms = rms;
    s_current_peak = peak;
    
    // Call registered callback if any
    if (s_meter_callback) {
        s_meter_callback(rms, peak, s_meter_user_data);
    }
}

/**
 * @brief Touch event callback for testing
 */
static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "Touch pressed on test area");
        // Note: Event callbacks run in LVGL context, no locking needed
        lv_obj_set_style_bg_color(obj, lv_color_make(0xFF, 0xEB, 0x3B), LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label) {
            lv_label_set_text(label, "Touch Detected!\nKeep pressing...");
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);  // Normal font
        }
    } else if (code == LV_EVENT_RELEASED) {
        ESP_LOGI(TAG, "Touch released from test area");
        // Note: Event callbacks run in LVGL context, no locking needed
        lv_obj_set_style_bg_color(obj, lv_color_make(0xE3, 0xF2, 0xFD), LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label) {
            lv_label_set_text(label, "Touch Test Area\nTap here to test touchscreen!");
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);  // Normal font
        }
    }
}

/**
 * @brief Mark content as changed to trigger refresh
 */
void wavex_ui_mark_content_changed(void)
{
    s_content_changed = true;
}

/**
 * @brief Adaptive refresh rate control for optimal performance
 */
static void adaptive_refresh_control(void)
{
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000); // Convert to ms
    
    // Only refresh if content has changed and enough time has passed
    if (s_content_changed) {
        uint32_t time_since_last_refresh = current_time - s_last_refresh_time;
        
        // Use minimum refresh interval for responsive updates
        if (time_since_last_refresh >= MIN_REFRESH_INTERVAL_MS) {
            LV_LOCK();
            lv_refr_now(s_lvgl_display);
            LV_UNLOCK();
            
            s_content_changed = false;
            s_last_refresh_time = current_time;
            s_refresh_count++;
            
            // Log refresh rate every 100 refreshes for monitoring
            if (s_refresh_count % 100 == 0) {
                ESP_LOGD(TAG, "Display refresh count: %lu", s_refresh_count);
            }
        }
    }
}

/**
 * @brief Initialize LVGL display using BSP (Recommended approach)
 */
 static esp_err_t init_lvgl_display(void)
 {
     ESP_LOGI(TAG, "Initializing LVGL display using BSP...");

    // Initialize LVGL core
    ESP_LOGI(TAG, "Initializing LVGL...");
    lv_init();

    // Check available memory before display initialization
    ESP_LOGI(TAG, "Memory before display init:");
    ESP_LOGI(TAG, "  Free heap: %zu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Minimum free heap: %zu bytes", esp_get_minimum_free_heap_size());

    // Use BSP's LVGL display setup with optimized config for higher refresh rates
    ESP_LOGI(TAG, "Using BSP's LVGL display setup with optimized buffer configuration...");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 720 * 20,   // Optimized: 20 lines × 720px × 2B = 28.8KB
        .double_buffer = true,     // Enable double buffering for smoother updates
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,   // Enable software rotation for HX8394
        }
    };

    s_lvgl_display = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(s_lvgl_display, ESP_FAIL, TAG, "Failed to start BSP display");

    // BSP disabled mirror operations for HX8394, so we need software rotation
    // Rotate 90 degrees clockwise
    LV_LOCK();
    lv_display_set_rotation(s_lvgl_display, LV_DISPLAY_ROTATION_90);
    LV_UNLOCK();

    // BSP handles touch setup automatically
    ESP_LOGI(TAG, "LVGL display initialized successfully");
    return ESP_OK;
 }
 
/**
 * @brief Create the hotkey region with 6 buttons at bottom of screen
 */
static void create_hotkey_region(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_hotkey_region: parent is NULL");
        return;
    }

    ESP_LOGI(TAG, "Creating hotkey region...");
    LV_LOCK();

    // Create hotkey region container with dark mode styling
    s_hotkey_region = lv_obj_create(parent);
    lv_obj_set_size(s_hotkey_region, lv_pct(100), 100);  // 100px tall, full width
    lv_obj_set_style_bg_color(s_hotkey_region, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Black background
    lv_obj_set_style_border_width(s_hotkey_region, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_hotkey_region, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN); // Dark gray border
    lv_obj_set_style_pad_all(s_hotkey_region, 5, LV_PART_MAIN);
    lv_obj_align(s_hotkey_region, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Set flex layout for horizontal arrangement of buttons
    lv_obj_set_flex_flow(s_hotkey_region, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hotkey_region, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create 6 hotkey buttons with dynamic sizing (will be updated by update_hotkey_labels)
    for (int i = 0; i < 6; i++) {
        s_hotkey_buttons[i] = lv_btn_create(s_hotkey_region);
        lv_obj_set_size(s_hotkey_buttons[i], lv_pct(16), 90);  // Default 16% width, will be updated dynamically
        
        // Blue button styling with white text (matching main screen buttons)
        lv_obj_set_style_bg_color(s_hotkey_buttons[i], lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
        lv_obj_set_style_bg_color(s_hotkey_buttons[i], lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
        lv_obj_set_style_border_width(s_hotkey_buttons[i], 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_hotkey_buttons[i], lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
        lv_obj_set_style_radius(s_hotkey_buttons[i], 5, LV_PART_MAIN);
        
        // Ensure button can receive input events
        lv_obj_clear_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_CLICKABLE);
        
        // Add event callbacks for all touch events
        lv_obj_add_event_cb(s_hotkey_buttons[i], hotkey_button_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(s_hotkey_buttons[i], hotkey_button_event_cb, LV_EVENT_PRESSED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(s_hotkey_buttons[i], hotkey_button_event_cb, LV_EVENT_RELEASED, (void*)(intptr_t)i);
        
        // Create label for button with white text
        s_hotkey_labels[i] = lv_label_create(s_hotkey_buttons[i]);
        lv_label_set_text(s_hotkey_labels[i], "");  // Empty initially
        lv_obj_set_style_text_font(s_hotkey_labels[i], &lv_font_montserrat_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hotkey_labels[i], lv_color_white(), LV_PART_MAIN); // White text
        lv_obj_center(s_hotkey_labels[i]);
    }

    LV_UNLOCK();
    ESP_LOGI(TAG, "Hotkey region created successfully with 6 buttons");
}

/**
 * @brief Update hotkey labels based on current screen context
 */
void wavex_ui_update_hotkey_labels(const char* labels[6])
{
    if (s_hotkey_labels[0] == NULL) {
        ESP_LOGW(TAG, "Hotkey labels not initialized");
        return;
    }

    // Count number of active labels
    int num_active_labels = 0;
    for (int i = 0; i < 6; i++) {
        if (labels[i] && strlen(labels[i]) > 0) {
            num_active_labels++;
        }
    }

    // Calculate dynamic button widths
    int button_widths[6];
    calculate_hotkey_layout(num_active_labels, button_widths);

    LV_LOCK();
    for (int i = 0; i < 6; i++) {
        if (s_hotkey_labels[i] && lv_obj_is_valid(s_hotkey_labels[i])) {
            if (labels[i] && strlen(labels[i]) > 0) {
                lv_label_set_text(s_hotkey_labels[i], labels[i]);
                lv_obj_clear_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_CLICKABLE);
                // Update button width based on calculated layout
                lv_obj_set_size(s_hotkey_buttons[i], lv_pct(button_widths[i]), 90);
            } else {
                lv_label_set_text(s_hotkey_labels[i], "");
                lv_obj_add_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_hotkey_buttons[i], LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
    LV_UNLOCK();
    
    ESP_LOGI(TAG, "Updated hotkey labels: %d active items, layout: %d%% %d%% %d%% %d%% %d%% %d%%", 
             num_active_labels, button_widths[0], button_widths[1], button_widths[2], 
             button_widths[3], button_widths[4], button_widths[5]);
}

/**
 * @brief Clear all hotkey labels
 */
static void clear_hotkey_labels(void)
{
    const char* empty_labels[6] = {"", "", "", "", "", ""};
    wavex_ui_update_hotkey_labels(empty_labels);
}

/**
 * @brief Calculate dynamic hotkey button layout based on number of items
 */
static void calculate_hotkey_layout(int num_items, int* button_widths)
{
    // 6 slots total, distribute based on number of items
    int slots_per_button = 6 / num_items;
    int remaining_slots = 6 % num_items;
    
    for (int i = 0; i < 6; i++) {
        if (i < num_items) {
            // Each button gets base slots plus potentially one extra slot
            int slots = slots_per_button + (i < remaining_slots ? 1 : 0);
            button_widths[i] = (slots * 100) / 6; // Convert to percentage
        } else {
            button_widths[i] = 0; // Hidden buttons
        }
    }
}

/**
 * @brief Set current screen context for hotkey mapping
 */
void wavex_ui_set_screen_context(const char* screen_name)
{
    s_current_screen = screen_name;
}

/**
 * @brief Update header title
 */
void wavex_ui_update_header_title(const char* title)
{
    if (s_header_title && lv_obj_is_valid(s_header_title)) {
        LV_LOCK();
        lv_label_set_text(s_header_title, title);
        LV_UNLOCK();
    }
}

/**
 * @brief Update hotkey label for a specific button
 */
void wavex_ui_update_hotkey_label(int button_index, const char* label)
{
    if (button_index < 0 || button_index >= 6 || !s_hotkey_labels[button_index]) {
        ESP_LOGW(TAG, "Invalid hotkey button index: %d", button_index);
        return;
    }
    
    LV_LOCK();
    if (lv_obj_is_valid(s_hotkey_labels[button_index])) {
        lv_label_set_text(s_hotkey_labels[button_index], label);
    }
    LV_UNLOCK();
}

/**
 * @brief Hotkey button event callback
 */
static void hotkey_button_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int button_index = (int)(intptr_t)lv_event_get_user_data(e);

    ESP_LOGI(TAG, "Hotkey button event: code=%d, button=%d, screen=%s", code, button_index, s_current_screen);

    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Hotkey button %d clicked on screen %s", button_index, s_current_screen);
        
        // Handle hotkey actions based on current screen context
        if (s_content_area == NULL || !lv_obj_is_valid(s_content_area)) {
            ESP_LOGE(TAG, "Content area not available for hotkey navigation");
            return;
        }
        
        // Lock LVGL for thread safety
        LV_LOCK();
        
        // Map button index to hotkey actions based on current screen
        if (strcmp(s_current_screen, "main") == 0) {
            // Main menu: Sample, System
            switch (button_index) {
                case 0: // Sample
                    ESP_LOGI(TAG, "Hotkey: Navigating to sample menu");
                    create_sample_menu(s_content_area);
                    break;
                case 1: // System
                    ESP_LOGI(TAG, "Hotkey: Navigating to system menu");
                    create_system_menu(s_content_area);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown hotkey button index for main menu: %d", button_index);
                    break;
            }
        } else if (strcmp(s_current_screen, "sample") == 0) {
            // Sample menu: Record, Edit, Load/Save, Back
            switch (button_index) {
                case 0: // Record
                    ESP_LOGI(TAG, "Hotkey: Record option selected");
                    // TODO: Implement record functionality
                    break;
                case 1: // Edit
                    ESP_LOGI(TAG, "Hotkey: Edit option selected");
                    // TODO: Implement edit functionality
                    break;
                case 2: // Load/Save
                    ESP_LOGI(TAG, "Hotkey: Load/Save option selected");
                    create_sample_load_save_page(s_content_area);
                    break;
                case 3: // Back
                    ESP_LOGI(TAG, "Hotkey: Navigating back to main menu");
                    create_main_menu(s_content_area);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown hotkey button index for sample menu: %d", button_index);
                    break;
            }
        } else if (strcmp(s_current_screen, "system") == 0) {
            // System menu: Diagnostics, Settings, Back
            switch (button_index) {
                case 0: // Diagnostics
                    ESP_LOGI(TAG, "Hotkey: Navigating to diagnostics page");
                    diagnostics_page_create(s_content_area);
                    break;
                case 1: // Settings
                    ESP_LOGI(TAG, "Hotkey: Settings option selected");
                    // TODO: Implement settings functionality
                    break;
                case 2: // Back
                    ESP_LOGI(TAG, "Hotkey: Navigating back to main menu");
                    create_main_menu(s_content_area);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown hotkey button index for system menu: %d", button_index);
                    break;
            }
        } else if (strcmp(s_current_screen, "diagnostics") == 0) {
            // Diagnostics page: Back
            switch (button_index) {
                case 0: // Back
                    ESP_LOGI(TAG, "Hotkey: Navigating back to system menu");
                    create_system_menu(s_content_area);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown hotkey button index for diagnostics page: %d", button_index);
                    break;
            }
        } else if (strcmp(s_current_screen, "sample_load_save") == 0) {
            // Sample Load/Save page: Audition, Load, Up, Down, Select, Back
            switch (button_index) {
                case 0: // Audition/Stop
                    ESP_LOGI(TAG, "Hotkey: Audition/Stop sample");
                    if (s_sample_load_save_page) {
                        if (s_sample_load_save_page->is_playing) {
                            // Stop current audition
                            ESP_LOGI(TAG, "Stopping current audition");
                            wavex_sample_load_save_stop_audition(s_sample_load_save_page);
                            // Don't update button text immediately - wait for stop response
                        } else {
                            // Start new audition
                            const wavex_file_entry_t* selected = wavex_file_browser_get_selected(s_sample_load_save_page->file_browser);
                            if (selected && !selected->is_directory) {
                                // Use index-based audition for better performance
                                uint32_t selected_index = wavex_file_browser_get_selected_index(s_sample_load_save_page->file_browser);
                                wavex_sample_load_save_audition_sample_by_index(s_sample_load_save_page, selected_index);
                                // Update button text to "Stop"
                                wavex_ui_update_hotkey_label(0, "Stop");
                            } else {
                                ESP_LOGW(TAG, "No valid file selected for audition");
                            }
                        }
                    }
                    break;
                case 1: // Load
                    ESP_LOGI(TAG, "Hotkey: Load sample");
                    if (s_sample_load_save_page) {
                        const wavex_file_entry_t* selected = wavex_file_browser_get_selected(s_sample_load_save_page->file_browser);
                        if (selected && !selected->is_directory) {
                            wavex_sample_load_save_load_sample(s_sample_load_save_page, selected->path);
                        }
                    }
                    break;
                case 2: // Up Arrow
                    ESP_LOGI(TAG, "Hotkey: Navigate up in file list");
                    if (s_sample_load_save_page && s_sample_load_save_page->file_browser) {
                        uint32_t current_idx = wavex_file_browser_get_selected_index(s_sample_load_save_page->file_browser);
                        if (current_idx > 0) {
                            wavex_file_browser_set_selection(s_sample_load_save_page->file_browser, current_idx - 1);
                            ESP_LOGI(TAG, "Navigated up to index %d", current_idx - 1);
                        }
                    }
                    break;
                case 3: // Down Arrow
                    ESP_LOGI(TAG, "Hotkey: Navigate down in file list");
                    if (s_sample_load_save_page && s_sample_load_save_page->file_browser) {
                        uint32_t current_idx = wavex_file_browser_get_selected_index(s_sample_load_save_page->file_browser);
                        uint32_t max_idx = wavex_file_browser_get_entry_count(s_sample_load_save_page->file_browser);
                        if (current_idx < max_idx - 1) {
                            wavex_file_browser_set_selection(s_sample_load_save_page->file_browser, current_idx + 1);
                            ESP_LOGI(TAG, "Navigated down to index %d", current_idx + 1);
                        }
                    }
                    break;
                case 4: // Select
                    ESP_LOGI(TAG, "Hotkey: Select action");
                    if (s_sample_load_save_page) {
                        const wavex_file_entry_t* selected = wavex_file_browser_get_selected(s_sample_load_save_page->file_browser);
                        if (selected) {
                            if (selected->is_directory) {
                                // Enter directory
                                ESP_LOGI(TAG, "Entering directory: %s", selected->name);
                                wavex_file_browser_navigate_to(s_sample_load_save_page->file_browser, selected->path);
                            } else {
                                // Show file metadata
                                ESP_LOGI(TAG, "Showing metadata for file: %s", selected->name);
                                wavex_sample_load_save_update_info(s_sample_load_save_page, selected);
                            }
                        }
                    }
                    break;
                case 5: // Back
                    ESP_LOGI(TAG, "Hotkey: Back action");
                    if (s_sample_load_save_page) {
                        // Try to navigate up one directory level first
                        if (wavex_file_browser_navigate_up(s_sample_load_save_page->file_browser)) {
                            ESP_LOGI(TAG, "Navigated up one directory level");
                        } else {
                            // If we're at root, go back to sample menu
                            ESP_LOGI(TAG, "At root directory, navigating back to sample menu");
                            wavex_sample_load_save_destroy(s_sample_load_save_page);
                            s_sample_load_save_page = NULL;
                            create_sample_menu(s_content_area);
                        }
                    } else {
                        ESP_LOGW(TAG, "Sample load/save page is NULL, cannot navigate back");
                    }
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown hotkey button index for sample load/save page: %d", button_index);
                    break;
            }
        } else {
            ESP_LOGW(TAG, "Unknown screen context for hotkey: %s", s_current_screen);
        }
    } else if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "Hotkey button %d pressed", button_index);
    } else if (code == LV_EVENT_RELEASED) {
        ESP_LOGI(TAG, "Hotkey button %d released", button_index);
    }
    
    // Unlock LVGL
    LV_UNLOCK();
}

/**
 * @brief Create the main menu with navigation buttons
 */
static void create_main_menu(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_main_menu: parent is NULL");
        return;
    }

    ESP_LOGI(TAG, "Creating main menu...");
    LV_LOCK();
    // Clear parent content
    lv_obj_clean(parent);

    // Set screen context for hotkey mapping
    wavex_ui_set_screen_context("main");
    
    // Update header title
    wavex_ui_update_header_title("Main Menu");

    // Create a flex container for menu buttons (optimized for landscape)
    lv_obj_t *menu_cont = lv_obj_create(parent);
    lv_obj_set_size(menu_cont, lv_pct(95), lv_pct(100));  // Use full height since no title/back button
    lv_obj_set_style_bg_color(menu_cont, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Black background for dark mode
    lv_obj_set_style_border_width(menu_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_cont, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN); // Dark gray border
    lv_obj_set_style_pad_all(menu_cont, 15, LV_PART_MAIN);  // Reduced padding
    lv_obj_center(menu_cont);

    // Set flex layout for landscape mode - horizontal layout for 2 buttons
    lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create Sample button with blue styling
    lv_obj_t *btn1 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn1, lv_pct(45), 120);  // Increased height for larger font
    lv_obj_add_event_cb(btn1, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"sample");
    lv_obj_set_style_bg_color(btn1, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn1, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn1, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn1, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn1, 5, LV_PART_MAIN);
    lv_obj_t *label1 = lv_label_create(btn1);
    lv_label_set_text(label1, "Sample");
    lv_obj_set_style_text_font(label1, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label1, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label1);

    // Create System button with blue styling
    lv_obj_t *btn2 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn2, lv_pct(45), 120);
    lv_obj_add_event_cb(btn2, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"system");
    lv_obj_set_style_bg_color(btn2, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn2, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn2, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn2, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn2, 5, LV_PART_MAIN);
    lv_obj_t *label2 = lv_label_create(btn2);
    lv_label_set_text(label2, "System");
    lv_obj_set_style_text_font(label2, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label2, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label2);
    
    // Update hotkey labels for main menu
    const char* main_menu_labels[6] = {"Sample", "System", "", "", "", ""};
    wavex_ui_update_hotkey_labels(main_menu_labels);
    
    LV_UNLOCK();
}

/**
 * @brief Create the sample submenu
 */
static void create_sample_menu(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_sample_menu: parent is NULL");
        return;
    }

    ESP_LOGI(TAG, "Creating sample menu...");
    LV_LOCK();
    // Clear parent content
    lv_obj_clean(parent);

    // Set screen context for hotkey mapping
    wavex_ui_set_screen_context("sample");
    
    // Update header title
    wavex_ui_update_header_title("Sample Menu");

    // Create a flex container for sample options (optimized for landscape)
    lv_obj_t *menu_cont = lv_obj_create(parent);
    lv_obj_set_size(menu_cont, lv_pct(95), lv_pct(100));  // Use full height since no title/back button
    lv_obj_set_style_bg_color(menu_cont, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Black background for dark mode
    lv_obj_set_style_border_width(menu_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_cont, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN); // Dark gray border
    lv_obj_set_style_pad_all(menu_cont, 15, LV_PART_MAIN);  // Reduced padding
    lv_obj_center(menu_cont);

    // Set flex layout for landscape mode - horizontal layout
    lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Sample options (side by side for landscape) - adjusted sizing with blue styling
    lv_obj_t *btn1 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn1, lv_pct(30), 100);  // Increased height for larger font
    lv_obj_add_event_cb(btn1, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"record");
    lv_obj_set_style_bg_color(btn1, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn1, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn1, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn1, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn1, 5, LV_PART_MAIN);
    lv_obj_t *label1 = lv_label_create(btn1);
    lv_label_set_text(label1, "Record");
    lv_obj_set_style_text_font(label1, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label1, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label1);

    lv_obj_t *btn2 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn2, lv_pct(30), 100);
    lv_obj_add_event_cb(btn2, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"edit");
    lv_obj_set_style_bg_color(btn2, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn2, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn2, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn2, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn2, 5, LV_PART_MAIN);
    lv_obj_t *label2 = lv_label_create(btn2);
    lv_label_set_text(label2, "Edit");
    lv_obj_set_style_text_font(label2, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label2, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label2);

    lv_obj_t *btn3 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn3, lv_pct(30), 100);
    lv_obj_add_event_cb(btn3, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"load_save");
    lv_obj_set_style_bg_color(btn3, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn3, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn3, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn3, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn3, 5, LV_PART_MAIN);
    lv_obj_t *label3 = lv_label_create(btn3);
    lv_label_set_text(label3, "Load/Save");
    lv_obj_set_style_text_font(label3, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label3, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label3);
    
    // Update hotkey labels for sample menu
    const char* sample_menu_labels[6] = {"Record", "Edit", "Load/Save", "Back", "", ""};
    wavex_ui_update_hotkey_labels(sample_menu_labels);
    
    LV_UNLOCK();
}

/**
 * @brief Create the system submenu
 */
static void create_system_menu(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_system_menu: parent is NULL");
        return;
    }

    ESP_LOGI(TAG, "Creating system menu...");
    LV_LOCK();
    // Clear parent content
    lv_obj_clean(parent);

    // Set screen context for hotkey mapping
    wavex_ui_set_screen_context("system");
    
    // Update header title
    wavex_ui_update_header_title("System Menu");

    // Create a flex container for system options (optimized for landscape)
    lv_obj_t *menu_cont = lv_obj_create(parent);
    lv_obj_set_size(menu_cont, lv_pct(95), lv_pct(100));  // Use full height since no title/back button
    lv_obj_set_style_bg_color(menu_cont, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Black background for dark mode
    lv_obj_set_style_border_width(menu_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_cont, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN); // Dark gray border
    lv_obj_set_style_pad_all(menu_cont, 15, LV_PART_MAIN);  // Reduced padding
    lv_obj_center(menu_cont);

    // Set flex layout for landscape mode - horizontal layout
    lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // System options (side by side for landscape) - adjusted sizing with blue styling
    lv_obj_t *btn1 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn1, lv_pct(45), 100);  // Increased height for larger font
    lv_obj_add_event_cb(btn1, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"diagnostics");
    lv_obj_set_style_bg_color(btn1, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn1, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn1, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn1, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn1, 5, LV_PART_MAIN);
    lv_obj_t *label1 = lv_label_create(btn1);
    lv_label_set_text(label1, "Diagnostics");
    lv_obj_set_style_text_font(label1, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label1, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label1);

    lv_obj_t *btn2 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn2, lv_pct(45), 100);
    lv_obj_add_event_cb(btn2, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"settings");
    lv_obj_set_style_bg_color(btn2, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN); // Blue background
    lv_obj_set_style_bg_color(btn2, lv_color_make(0x19, 0x76, 0xD2), LV_PART_MAIN | LV_STATE_PRESSED); // Darker blue when pressed
    lv_obj_set_style_border_width(btn2, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn2, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN); // Darker blue border
    lv_obj_set_style_radius(btn2, 5, LV_PART_MAIN);
    lv_obj_t *label2 = lv_label_create(btn2);
    lv_label_set_text(label2, "Settings");
    lv_obj_set_style_text_font(label2, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label2, lv_color_white(), LV_PART_MAIN); // White text
    lv_obj_center(label2);
    
    // Update hotkey labels for system menu
    const char* system_menu_labels[6] = {"Diagnostics", "Settings", "Back", "", "", ""};
    wavex_ui_update_hotkey_labels(system_menu_labels);
    
    LV_UNLOCK();
}

/**
 * @brief Create the sample load/save page
 */
static void create_sample_load_save_page(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_sample_load_save_page: parent is NULL");
        return;
    }

    ESP_LOGI(TAG, "Creating sample load/save page...");
    LV_LOCK();
    
    // Clear parent content
    lv_obj_clean(parent);

    // Set screen context for hotkey mapping
    wavex_ui_set_screen_context("sample_load_save");
    
    // Update header title
    wavex_ui_update_header_title("Sample Load/Save");

    // Destroy existing page if it exists
    if (s_sample_load_save_page) {
        wavex_sample_load_save_destroy(s_sample_load_save_page);
        s_sample_load_save_page = NULL;
    }

    // Create the proper sample load/save page with file browser
    s_sample_load_save_page = wavex_sample_load_save_create(parent);
    if (!s_sample_load_save_page) {
        ESP_LOGE(TAG, "Failed to create sample load/save page");
        // Fall back to test page
        lv_obj_t* container = lv_obj_create(parent);
        lv_obj_set_size(container, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(container, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
        lv_obj_set_style_border_width(container, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(container, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, 0);
        
        lv_obj_t* test_label = lv_label_create(container);
        lv_label_set_text(test_label, "Sample Load/Save Page\n\nFile Browser failed to load\n\nUse hotkeys: Audition, Load, Save, Back");
        lv_obj_set_style_text_color(test_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(test_label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_center(test_label);
    } else {
        ESP_LOGI(TAG, "Sample load/save page created successfully with file browser");
    }
    
    // Update hotkey labels for sample load/save page
    const char* load_save_labels[6] = {"Audition", "Load", "Up", "Down", "Select", "Back"};
    wavex_ui_update_hotkey_labels(load_save_labels);
    
    ESP_LOGI(TAG, "Sample load/save page creation completed");
    LV_UNLOCK();
}

/**
 * @brief Menu button event callback
 */
static void menu_button_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const char *btn_id = (const char *)lv_event_get_user_data(e);

    if (code == LV_EVENT_CLICKED) {
        if (btn_id == NULL) {
            ESP_LOGE(TAG, "Menu button clicked with NULL ID");
            return;
        }

        ESP_LOGI(TAG, "Menu button clicked: %s", btn_id);

        // Use the stored content area reference
        if (s_content_area == NULL) {
            ESP_LOGE(TAG, "Content area not initialized for menu navigation");
            return;
        }
        
        // Additional safety check
        if (!lv_obj_is_valid(s_content_area)) {
            ESP_LOGE(TAG, "Content area is not a valid LVGL object");
            return;
        }

        if (strcmp(btn_id, "sample") == 0) {
            ESP_LOGI(TAG, "Navigating to sample menu...");
            create_sample_menu(s_content_area);
        } else if (strcmp(btn_id, "system") == 0) {
            ESP_LOGI(TAG, "Navigating to system menu...");
            create_system_menu(s_content_area);
        } else if (strcmp(btn_id, "record") == 0) {
            ESP_LOGI(TAG, "Record option selected");
            // TODO: Implement record functionality
        } else if (strcmp(btn_id, "edit") == 0) {
            ESP_LOGI(TAG, "Edit option selected");
            // TODO: Implement edit functionality
        } else if (strcmp(btn_id, "load_save") == 0) {
            ESP_LOGI(TAG, "Load/Save option selected");
            create_sample_load_save_page(s_content_area);
        } else if (strcmp(btn_id, "settings") == 0) {
            ESP_LOGI(TAG, "Settings option selected");
            // TODO: Implement settings functionality
        } else if (strcmp(btn_id, "diagnostics") == 0) {
            ESP_LOGI(TAG, "Navigating to diagnostics page...");
            diagnostics_page_create(s_content_area);
        } else if (strcmp(btn_id, "back_main") == 0) {
            ESP_LOGI(TAG, "Navigating back to main menu...");
            create_main_menu(s_content_area);
        } else if (strcmp(btn_id, "back_system") == 0) {
            ESP_LOGI(TAG, "Navigating back to system menu...");
            create_system_menu(s_content_area);
        } else {
            ESP_LOGI(TAG, "Menu option '%s' selected", btn_id);
        }
    }
}

/**
 * @brief UI task implementation
 */
 static void ui_task(void *pvParameters)
 {
    ESP_LOGI(TAG, "UI task started with full UI support");

    // Initialize LVGL display
    ESP_LOGI(TAG, "Initializing LVGL display...");

    // Initialize LVGL display
    esp_err_t lvgl_ret = init_lvgl_display();
    if (lvgl_ret != ESP_OK) {
         ESP_LOGE(TAG, "Failed to initialize LVGL display: %s", esp_err_to_name(lvgl_ret));
         vTaskDelete(NULL);
         return;
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    // Log memory status
    ESP_LOGI(TAG, "Memory after display init:");
    ESP_LOGI(TAG, "  Free heap: %zu bytes", esp_get_free_heap_size());

    // Create the main UI with dark mode
    ESP_LOGI(TAG, "Creating main UI...");
    LV_LOCK();
    lv_obj_t *main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(main_screen, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Black background for dark mode
    
    // Create header (increased size by 50% for better visibility)
    lv_obj_t *header = lv_obj_create(main_screen);
    lv_obj_set_size(header, lv_pct(100), 75);  // Increased from 50 to 75 (50% increase)
    lv_obj_set_style_bg_color(header, lv_color_make(0x2E, 0x34, 0x40), LV_PART_MAIN); // Keep dark gray for contrast
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    
    // Add title to header (larger font to match increased header size)
    s_header_title = lv_label_create(header);
    lv_label_set_text(s_header_title, "Main Menu");
    lv_obj_set_style_text_color(s_header_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_header_title, &lv_font_montserrat_32, LV_PART_MAIN); // Increased from 22 to 32
    lv_obj_center(s_header_title);
    
    // Create content area (adjusted for landscape mode with hotkey region) with dark mode
    lv_obj_t *content = lv_obj_create(main_screen);
    lv_obj_set_size(content, lv_pct(100), 545);  // 720 - 75 - 100 = 545 pixels (full height minus header and hotkey region)
    lv_obj_set_style_bg_color(content, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN); // Black background for dark mode
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 75);  // Position below header
    
    // Store content area reference for menu navigation
    s_content_area = content;
    
    // Create hotkey region at bottom of screen
    create_hotkey_region(main_screen);
    
    // Create main menu
    create_main_menu(content);
    LV_UNLOCK();

    ESP_LOGI(TAG, "Main UI created successfully");

    // Initialize diagnostics page module (starts its timer)
    diagnostics_page_init();

    // Create meter update timer (every 33ms for 30 FPS real-time updates)
    const esp_timer_create_args_t meter_timer_args = {
        .callback = &meter_update_cb,
        .name = "meter_timer"
    };
    esp_err_t timer_ret = esp_timer_create(&meter_timer_args, &s_meter_timer_handle);
    if (timer_ret == ESP_OK) {
        esp_timer_start_periodic(s_meter_timer_handle, 33000); // 33ms in microseconds (30 FPS)
        ESP_LOGI(TAG, "Meter timer started (33ms real-time updates - 30 FPS)");
    } else {
        ESP_LOGE(TAG, "Failed to create meter timer: %s", esp_err_to_name(timer_ret));
    }

    // Register meter data callback for real-time audio meter updates
    inter_mcu_set_meter_listener(meter_data_cb, NULL);
    ESP_LOGI(TAG, "Meter data callback registered");
    
    // Test meter callback with dummy data to verify it's working
    ESP_LOGI(TAG, "Testing meter callback with dummy data...");
    meter_data_cb(0.5f, 0.8f, NULL); // Test with 50% RMS, 80% peak

    // Start TCA8418 keypad on BSP I2C; INT on GPIO31 per pin_config
    {
        const int tca_int_gpio = WAVEX_ESP_BTN_INT; // GPIO31
        const uint8_t tca_addr = 0x34;
        esp_err_t kret = wavex_ui::tca8418_keypad_start(tca_int_gpio, tca_addr);
        if (kret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start TCA8418 keypad: %s", esp_err_to_name(kret));
        } else {
            ESP_LOGI(TAG, "TCA8418 keypad started (INT GPIO=%d, addr=0x%02X)", tca_int_gpio, tca_addr);
        }
    }

    // Set initial input context (demo)
    wavex_ui::InputDispatcher::instance().setActiveContext(wavex_ui::createPatchListContext());

    // Main UI loop with adaptive refresh rate control
    ESP_LOGI(TAG, "UI loop started with adaptive refresh rate control");
    while (1) {
        // Apply encoder movement to active UI when applicable
        int32_t enc_delta = pcnt_consume_delta(WAVEX_ENCODER_PCNT_UNIT);
        if (enc_delta != 0) {
            // Post unified input events for encoder movement
            wavex_ui::InputEvent evt;
            evt.type = (enc_delta > 0) ? wavex_ui::InputType::EncoderRight : wavex_ui::InputType::EncoderLeft;
            evt.delta = (int16_t)enc_delta;
            evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            wavex_ui::InputDispatcher::instance().post(evt);

            // Currently, only the Sample Load/Save page has a file browser
            if (strcmp(s_current_screen, "sample_load_save") == 0 && s_sample_load_save_page && s_sample_load_save_page->file_browser) {
                uint32_t current_idx = wavex_file_browser_get_selected_index(s_sample_load_save_page->file_browser);
                uint32_t max_idx = wavex_file_browser_get_entry_count(s_sample_load_save_page->file_browser);
                int32_t target = (int32_t)current_idx + enc_delta;
                if (target < 0) target = 0;
                if (max_idx > 0 && target > (int32_t)max_idx - 1) target = (int32_t)max_idx - 1;
                if ((uint32_t)target != current_idx) {
                    wavex_file_browser_set_selection(s_sample_load_save_page->file_browser, (uint32_t)target);
                }
            }
        }
        // Dispatch queued input events to current context
        wavex_ui::InputDispatcher::instance().processAll();
        // Process deferred diagnostics updates (prevents deadlock)
        diagnostics_page_process_deferred_updates();
        
        // Process deferred meter updates (prevents deadlock during audio playback)
        process_deferred_meter_updates();
        
        // Use adaptive refresh control for optimal performance
        adaptive_refresh_control();
        
        // Short delay to prevent excessive CPU usage
        vTaskDelay(pdMS_TO_TICKS(32)); // 32ms delay for 30 FPS theoretical maximum
    }
 }
 
 esp_err_t wavex_ui_task_start(void)
 {
     ESP_LOGI(TAG, "Starting UI task with MIPI DSI display and LVGL...");
     
    // Create UI task on Core 1 (different from SPI which typically runs on Core 0)
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        ui_task,
        "ui_task",
        16384,  // Increased stack size to 16KB for LVGL and diagnostics
        NULL,
        2,     // Priority
        &s_ui_task_handle,
        1      // Run on Core 1
    );
     
     if (task_ret != pdPASS) {
         ESP_LOGE(TAG, "Failed to create UI task");
         return ESP_FAIL;
     }
     
     ESP_LOGI(TAG, "UI task started successfully");
     return ESP_OK;
 }
 
 esp_err_t wavex_ui_task_stop(void)
 {
     ESP_LOGI(TAG, "Stopping UI task...");
     
     if (s_ui_task_handle != NULL) {
         vTaskDelete(s_ui_task_handle);
         s_ui_task_handle = NULL;
     }

    // Stop and delete the LVGL tick timer
    if (s_lvgl_tick_timer_handle) {
        esp_timer_stop(s_lvgl_tick_timer_handle);
        esp_timer_delete(s_lvgl_tick_timer_handle);
        s_lvgl_tick_timer_handle = NULL;
    }

    // Stop diagnostics page module
    diagnostics_page_stop();

    // Stop and delete the meter timer
    if (s_meter_timer_handle) {
        esp_timer_stop(s_meter_timer_handle);
        esp_timer_delete(s_meter_timer_handle);
        s_meter_timer_handle = NULL;
    }
     
     // Deinitialize LVGL
     if (s_lvgl_display) {
         lv_display_delete(s_lvgl_display);
         s_lvgl_display = NULL;
     }
     
     // Deinitialize touch controller (BSP will handle this)
     if (s_touch_handle) {
         esp_lcd_touch_del(s_touch_handle);
         s_touch_handle = NULL;
     }

     // BSP handles I2C bus cleanup automatically
     
     // Deinitialize display using BSP
     // BSP handles display cleanup automatically when LVGL display is deleted
     // No need for manual panel deinitialization
     
     ESP_LOGI(TAG, "UI task stopped");
     return ESP_OK;
 }
 
esp_err_t wavex_ui_get_panel_handle(esp_lcd_panel_handle_t *panel_handle)
{
    if (panel_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get panel handle from BSP if not already available
    if (s_panel_handle == NULL) {
        ESP_LOGI(TAG, "Getting panel handle from BSP...");
        bsp_display_config_t config = {0}; // Default config
        ESP_RETURN_ON_ERROR(bsp_display_new(&config, &s_panel_handle, NULL),
                           TAG, "Failed to get panel handle from BSP");
    }

    *panel_handle = s_panel_handle;
    return ESP_OK;
}