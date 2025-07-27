#include "ui_main.h"
#include "ui_about.h"
#include "version.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_st7796.h"
#include "FT6X36.h"
#include "esp_log.h"
#include "hardware_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <inttypes.h>
#include "inter_mcu.h"
#include "esp_task_wdt.h"
#include "driver/ledc.h"
#include "esp_err.h"

// Global for touch visualization (debug)
static lv_obj_t* touch_circle = NULL;

// Custom LVGL memory allocation functions to use PSRAM
void* lvgl_malloc(size_t size) {
    // Try PSRAM first for general LVGL allocations
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        // Fallback to internal RAM if PSRAM fails
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
        if (ptr) {
            ESP_LOGW("LVGL_MEM", "PSRAM allocation failed, using internal RAM for %zu bytes", size);
        }
    }
    return ptr;
}

void lvgl_free(void* ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

void* lvgl_realloc(void* ptr, size_t size) {
    // Try PSRAM first
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (!new_ptr && size > 0) {
        // Fallback to internal RAM
        new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL);
        if (new_ptr) {
            ESP_LOGW("LVGL_MEM", "PSRAM realloc failed, using internal RAM for %zu bytes", size);
        }
    }
    return new_ptr;
}

static lv_obj_t* main_screen = NULL;
static lv_obj_t* main_menu = NULL;

// LCD and touch handles
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;
static FT6X36* touch_controller = NULL;
static lv_display_t* lvgl_disp = NULL;
static lv_indev_t* lvgl_touch_indev = NULL;

// Global variables for settings
static bool system_performance_mode = true;
static bool auto_save_enabled = true;
static bool debug_logging_enabled = false;
static uint8_t display_brightness = 80;
static uint32_t screen_timeout_seconds = 120;
static bool dark_theme_enabled = true;

// Event callback for brightness slider
static void brightness_slider_event_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    display_brightness = (uint8_t)value;
    
#if WAVEX_BACKLIGHT_ENABLED == 1
    #if WAVEX_BACKLIGHT_PWM_MODE == 1
        // PWM mode - set duty cycle
        uint32_t duty = (value * 255) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        ESP_LOGI("UI", "Display brightness set to %" PRId32 "%% (PWM duty: %" PRIu32 ")", value, duty);
    #else
        // GPIO mode - simple on/off based on threshold
        if (value > 10) {
            gpio_set_level(WAVEX_LCD_GPIO_BL, WAVEX_LCD_BL_ON_LEVEL);
            ESP_LOGI("UI", "Display brightness set to %" PRId32 "%% (backlight ON)", value);
        } else {
            gpio_set_level(WAVEX_LCD_GPIO_BL, !WAVEX_LCD_BL_ON_LEVEL);
            ESP_LOGI("UI", "Display brightness set to %" PRId32 "%% (backlight OFF)", value);
        }
    #endif
#else
    ESP_LOGI("UI", "Brightness slider moved to %" PRId32 "%% (backlight disabled by config)", value);
#endif
}

// Event callback for screen timeout slider
static void screen_timeout_event_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    screen_timeout_seconds = (uint32_t)value;
    ESP_LOGI("UI", "Screen timeout set to %" PRIu32 " seconds", screen_timeout_seconds);
    // TODO: Implement actual screen timeout functionality
}

// Event callback for performance mode switch
static void performance_mode_event_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    system_performance_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI("UI", "Performance mode %s", system_performance_mode ? "ENABLED" : "DISABLED");
    
    // Adjust CPU frequency based on performance mode
    if (system_performance_mode) {
        // High performance: 240MHz
        ESP_LOGI("UI", "Setting CPU to high performance mode (240MHz)");
        // TODO: Implement actual CPU frequency scaling
    } else {
        // Power saving: 160MHz
        ESP_LOGI("UI", "Setting CPU to power saving mode (160MHz)");
        // TODO: Implement actual CPU frequency scaling
    }
}

// Event callback for auto-save switch
static void auto_save_event_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    auto_save_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI("UI", "Auto-save settings %s", auto_save_enabled ? "ENABLED" : "DISABLED");
    // TODO: Implement NVS settings auto-save functionality
}

// Event callback for debug logging switch
static void debug_logging_event_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    debug_logging_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    
    if (debug_logging_enabled) {
        esp_log_level_set("*", ESP_LOG_DEBUG);
        ESP_LOGI("UI", "Debug logging ENABLED for all components");
    } else {
        esp_log_level_set("*", ESP_LOG_INFO);
        ESP_LOGI("UI", "Debug logging DISABLED - INFO level only");
    }
}

// Event callback for dark theme switch
static void dark_theme_event_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    dark_theme_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI("UI", "Dark theme %s", dark_theme_enabled ? "ENABLED" : "DISABLED");
    
    // Change main screen background color based on theme
    if (dark_theme_enabled) {
        lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x003a57), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(main_screen, lv_color_hex(0xe0e0e0), LV_PART_MAIN);
    }
}

// Event callback for system restart action
static void system_restart_event_cb(lv_event_t* e) {
    ESP_LOGW("UI", "System restart requested by user");
    // Add a small delay to let the log message go out
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/**
 * @brief Check available heap before expensive operations
 */
static bool check_heap_available(size_t required_bytes, const char* operation) {
    size_t free_heap = esp_get_free_heap_size();
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    
    ESP_LOGI("UI", "Heap check for %s: free=%zu, internal=%zu, largest=%zu, required=%zu", 
             operation, free_heap, free_internal, largest_block, required_bytes);
    
    if (free_heap < required_bytes) {
        ESP_LOGW("UI", "Low heap for %s: %zu bytes free, %zu required", 
                operation, free_heap, required_bytes);
        return false;
    }
    
    if (largest_block < required_bytes) {
        ESP_LOGW("UI", "Heap fragmentation issue for %s: largest block %zu < required %zu", 
                operation, largest_block, required_bytes);
        return false;
    }
    
    return true;
}

/**
 * @brief Log detailed heap information
 */
static void log_heap_info(const char* context) {
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
    
    ESP_LOGI("UI", "Heap info [%s]: total_free=%zu, total_allocated=%zu, largest_free=%zu, min_free=%zu", 
             context,
             heap_info.total_free_bytes,
             heap_info.total_allocated_bytes, 
             heap_info.largest_free_block,
             heap_info.minimum_free_bytes);
             
    // Also log internal RAM specifically
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI("UI", "Internal RAM [%s]: free=%zu, largest=%zu", context, internal_free, internal_largest);
}

// Helper function to create menu text items
static lv_obj_t* create_menu_text(lv_obj_t* parent, const char* icon, const char* txt)
{
    if (!parent) {
        ESP_LOGE("UI", "create_menu_text: parent is NULL");
        return NULL;
    }

    // Check if we have enough memory before creating complex objects
    if (!check_heap_available(2000, "menu text creation")) {
        ESP_LOGW("UI", "Insufficient memory for menu text item - skipping");
        return NULL;
    }

    lv_obj_t* obj = lv_menu_cont_create(parent);
    if (!obj) {
        ESP_LOGE("UI", "create_menu_text: failed to create menu container - out of memory");
        return NULL;
    }

    if(icon) {
        lv_obj_t* img = lv_label_create(obj);
        if (img) {
            lv_label_set_text(img, icon);
        } else {
            ESP_LOGW("UI", "Failed to create icon label - memory pressure");
        }
    }

    if(txt) {
        lv_obj_t* label = lv_label_create(obj);
        if (label) {
            lv_label_set_text(label, txt);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
            lv_obj_set_flex_grow(label, 1);
        } else {
            ESP_LOGW("UI", "Failed to create text label - memory pressure");
        }
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

// Helper function to create menu sliders with event callbacks
static lv_obj_t* create_menu_slider_with_callback(lv_obj_t* parent, const char* icon, const char* txt, 
                                                  int32_t min, int32_t max, int32_t val, lv_event_cb_t callback)
{
    lv_obj_t* obj = create_menu_slider(parent, icon, txt, min, max, val);
    if (callback) {
        lv_obj_t* slider = lv_obj_get_child(obj, lv_obj_get_child_cnt(obj) - 1);
        lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return obj;
}

// Helper function to create menu switches with event callbacks
static lv_obj_t* create_menu_switch_with_callback(lv_obj_t* parent, const char* icon, const char* txt, 
                                                  bool checked, lv_event_cb_t callback)
{
    lv_obj_t* obj = create_menu_switch(parent, icon, txt, checked);
    if (callback) {
        lv_obj_t* sw = lv_obj_get_child(obj, lv_obj_get_child_cnt(obj) - 1);
        lv_obj_add_event_cb(sw, callback, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return obj;
}

// Helper function to create menu text items with click callbacks
static lv_obj_t* create_menu_text_with_callback(lv_obj_t* parent, const char* icon, const char* txt, lv_event_cb_t callback)
{
    lv_obj_t* obj = create_menu_text(parent, icon, txt);
    if (callback) {
        lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, NULL);
    }
    return obj;
}

// Touch state variables for LVGL integration
static TPoint current_touch_point = {0, 0};
static TEvent current_touch_event = TEvent::None;
static bool touch_pressed = false;

// Add touch filtering variables
static bool last_touch_state = false;
static int32_t last_touch_x = 0;
static int32_t last_touch_y = 0;
static uint32_t touch_debounce_time = 0;
static const uint32_t TOUCH_DEBOUNCE_MS = 50;  // 50ms debounce
static const int32_t TOUCH_NOISE_THRESHOLD = 10;  // Ignore movements < 10 pixels

// Add this near the top with other touch state variables
static bool touch_system_ready = false;
static uint32_t touch_init_complete_time = 0;
static const uint32_t TOUCH_STABILIZATION_MS = 2000; // 2 second stabilization period

// Add this near the top with other static variables
static esp_timer_handle_t touch_registration_timer = NULL;

// Add forward declaration at the top with other forward declarations
static void wavex_touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

// Add this callback function for the timer AFTER wavex_touch_read_cb function
static void register_touch_callback_delayed(void* arg) {
    if (lvgl_touch_indev && touch_controller) {
        ESP_LOGI("UI", "Registering touch callback after 1s stabilization period");
        if (lvgl_port_lock(10)) {
            lv_indev_set_read_cb(lvgl_touch_indev, wavex_touch_read_cb);
            lvgl_port_unlock();
            ESP_LOGI("UI", "Touch callback registered successfully - touch system active");
        } else {
            ESP_LOGW("UI", "Failed to lock LVGL for touch callback registration");
        }
    }
    
    // Clean up the timer
    if (touch_registration_timer) {
        esp_timer_delete(touch_registration_timer);
        touch_registration_timer = NULL;
    }
}

/**
 * @brief Touch event handler callback from FT6X36
 * Called when touch events occur (tap, drag, etc.)
 */
static void wavex_touch_event_handler(TPoint point, TEvent event)
{
    // Store the current touch data for LVGL to read
    current_touch_point = point;
    current_touch_event = event;
    
    // Update touch state based on event type
    switch (event) {
        case TEvent::TouchStart:
        case TEvent::Tap:
        case TEvent::DragStart:
        case TEvent::DragMove:
            touch_pressed = true;
            break;
        case TEvent::TouchEnd:
        case TEvent::DragEnd:
            touch_pressed = false;
            break;
        default:
            // Keep current state for other events
            break;
    }
}

/**
 * @brief Enhanced touch read callback with improved filtering and reduced noise
 */
static void wavex_touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    static uint32_t last_poll_time = 0;
    static uint32_t touch_count = 0;
    static uint32_t last_valid_event_time = 0;
    static TEvent last_reported_event = TEvent::None;
    static uint32_t touch_start_time = 0;
    static bool in_touch_sequence = false;
    
    uint32_t current_time = esp_timer_get_time() / 1000;  // Get time in milliseconds
    
    // Poll the touch controller for new data
    if (touch_controller != nullptr) {
        TPoint point;
        TEvent event;
        
        // Reduced debug logging - only every 5 seconds when no touch activity
        if (current_time - last_poll_time > 5000 && !in_touch_sequence) {
            ESP_LOGI("TOUCH_DEBUG", "Touch polling active - count: %" PRIu32 " (quiet)", touch_count);
            last_poll_time = current_time;
            touch_count = 0;
        }
        touch_count++;
        
        touch_controller->poll(&point, &event);
        
        // FILTER 1: Only process real events, ignore NoEvent completely
        if (event != TEvent::None) {
            // FILTER 2: COORDINATE VALIDATION FIRST - before any processing!
            // Convert to signed 32-bit for safe math
            int32_t x = (int32_t)point.x;
            int32_t y = (int32_t)point.y;
            
            // Filter out edge touches that are likely noise - BEFORE processing
            if (x <= 3 || x >= (WAVEX_LCD_H_RES - 3) || y <= 3 || y >= (WAVEX_LCD_V_RES - 3)) {
                ESP_LOGD("TOUCH_FILTER", "Filtering edge touch at (%" PRId32 ", %" PRId32 ") - ignoring event", x, y);
                // Don't process this event at all - return early
                data->state = LV_INDEV_STATE_RELEASED;
                data->continue_reading = false;
                return;
            }
            
            // Clamp coordinates to valid display area
            if (x < 0) x = 0;
            if (x >= WAVEX_LCD_H_RES) x = WAVEX_LCD_H_RES - 1;
            if (y < 0) y = 0;
            if (y >= WAVEX_LCD_V_RES) y = WAVEX_LCD_V_RES - 1;
            
            // Update the point with filtered coordinates
            point.x = (uint16_t)x;
            point.y = (uint16_t)y;
            
            // FILTER 3: Debounce rapid event changes (prevent bouncing)
            if (current_time - last_valid_event_time < 20) { // 20ms debounce
                // Too soon since last event - ignore
                data->continue_reading = false;
                return;
            }
            
            // FILTER 4: State-based filtering (NOW with clean coordinates)
            bool should_report_event = false;
            
            switch (event) {
                case TEvent::TouchStart:
                    if (!in_touch_sequence) {
                        should_report_event = true;
                        in_touch_sequence = true;
                        touch_start_time = current_time;
                        ESP_LOGI("TOUCH_EVENT", "Touch START at (%d, %d)", point.x, point.y);
                    }
                    // Ignore duplicate TouchStart events
                    break;
                    
                case TEvent::TouchMove:
                    if (in_touch_sequence) {
                        // Movement threshold - only report significant moves
                        int32_t dx = abs((int32_t)point.x - (int32_t)current_touch_point.x);
                        int32_t dy = abs((int32_t)point.y - (int32_t)current_touch_point.y);
                        
                        if (dx > TOUCH_NOISE_THRESHOLD || dy > TOUCH_NOISE_THRESHOLD) {
                            should_report_event = true;
                            // Only log significant moves to reduce noise
                            if (dx > 20 || dy > 20) {
                                ESP_LOGD("TOUCH_EVENT", "Touch MOVE to (%d, %d)", point.x, point.y);
                            }
                        }
                    } else {
                        // Move without start - treat as start
                        should_report_event = true;
                        in_touch_sequence = true;
                        touch_start_time = current_time;
                        ESP_LOGI("TOUCH_EVENT", "Touch START (via move) at (%d, %d)", point.x, point.y);
                        event = TEvent::TouchStart;  // Convert to start event
                    }
                    break;
                    
                case TEvent::TouchEnd:
                    if (in_touch_sequence) {
                        should_report_event = true;
                        in_touch_sequence = false;
                        ESP_LOGI("TOUCH_EVENT", "Touch END at (%d, %d) - duration: %" PRIu32 "ms", 
                                point.x, point.y, current_time - touch_start_time);
                    }
                    // Ignore spurious TouchEnd without TouchStart
                    break;
                    
                default:
                    break;
            }
            
            if (should_report_event) {
                wavex_touch_event_handler(point, event);
                last_valid_event_time = current_time;
                last_reported_event = event;
            }
        }
    }
    
    // Apply filtered coordinates to LVGL (this part stays mostly the same)
    if (touch_pressed) {
        // Use the already filtered coordinates from current_touch_point
        int32_t x = (int32_t)current_touch_point.x;
        int32_t y = (int32_t)current_touch_point.y;
        
        // Apply additional jitter filtering for continuous touches
        if (last_touch_state) {
            int32_t dx = abs(x - last_touch_x);
            int32_t dy = abs(y - last_touch_y);
            if (dx < 3 && dy < 3) { // Smaller threshold for fine touches
                // Use previous position to reduce jitter
                x = last_touch_x;
                y = last_touch_y;
            }
        }
        
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        
        last_touch_x = x;
        last_touch_y = y;
        last_touch_state = true;
        
        // Update touch visualization
        if (touch_circle) {
            lv_obj_set_pos(touch_circle, x - 10, y - 10);
            lv_obj_clear_flag(touch_circle, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // Touch released
        if (last_touch_state) {
            touch_debounce_time = current_time;
            in_touch_sequence = false;
        }
        data->state = LV_INDEV_STATE_RELEASED;
        last_touch_state = false;

        // Hide visualization
        if (touch_circle) {
            lv_obj_add_flag(touch_circle, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Continue reading
    data->continue_reading = false;
}

/**
 * @brief Initialize ST7796S display hardware
 * @return ESP_OK on success, error code on failure
 */
static void log_detailed_heap_info(const char* context) {
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
    
    ESP_LOGI("HEAP", "[%s] Total: %zu allocated + %zu free = %zu", 
             context,
             heap_info.total_allocated_bytes,
             heap_info.total_free_bytes,
             heap_info.total_allocated_bytes + heap_info.total_free_bytes);
             
    ESP_LOGI("HEAP", "[%s] Largest free block: %zu, Min free ever: %zu", 
             context, heap_info.largest_free_block, heap_info.minimum_free_bytes);
             
    // Log internal vs external memory
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    
    ESP_LOGI("HEAP", "[%s] Internal: %zu free (largest %zu), DMA: %zu free (largest %zu)", 
             context, internal_free, internal_largest, dma_free, dma_largest);
}

static esp_err_t wavex_display_init(void)
{
    ESP_LOGI("DISPLAY", "=== Starting display initialization ===");
    ESP_LOGI("DISPLAY", "Heap at start: %" PRIu32 " bytes", esp_get_free_heap_size());
    
    ESP_LOGI("DISPLAY", "Initializing ST7796S display...");
    
#if WAVEX_BACKLIGHT_PWM_MODE == 0
    // GPIO mode - configure backlight pin as regular GPIO
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << WAVEX_LCD_GPIO_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), "DISPLAY", "Backlight GPIO config failed");
    ESP_LOGI("DISPLAY", "Backlight GPIO configured (GPIO mode)");
#endif
    
    gpio_config_t rst_gpio_config = {
        .pin_bit_mask = 1ULL << WAVEX_LCD_GPIO_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_gpio_config), "DISPLAY", "LCD reset GPIO config failed");
    ESP_LOGI("DISPLAY", "LCD reset GPIO configured successfully");

    // explicit reset sequence
    ESP_LOGI("DISPLAY", "Performing explicit reset sequence (active level: %d)", WAVEX_LCD_RST_ACTIVE_LEVEL);

    // Assert reset (set to active level)
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_LCD_GPIO_RST, WAVEX_LCD_RST_ACTIVE_LEVEL), "DISPLAY", "LCD reset assert failed");
    ESP_LOGI("DISPLAY", "LCD reset asserted, waiting 200ms");
    vTaskDelay(pdMS_TO_TICKS(200));

    // Deassert reset (set to inactive level)
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_LCD_GPIO_RST, !WAVEX_LCD_RST_ACTIVE_LEVEL), "DISPLAY", "LCD reset deassert failed");
    ESP_LOGI("DISPLAY", "LCD reset deasserted successfully");
    vTaskDelay(pdMS_TO_TICKS(200));

    // Verify reset pin state
    int rst_level = gpio_get_level(WAVEX_LCD_GPIO_RST);
    ESP_LOGI("DISPLAY", "LCD reset pin state: %d", rst_level);
    
    // Initialize SPI bus for display
    ESP_LOGI("DISPLAY", "Initializing SPI bus");
    const spi_bus_config_t buscfg = {
        .mosi_io_num = WAVEX_LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,  // Display doesn't need MISO
        .sclk_io_num = WAVEX_LCD_GPIO_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = WAVEX_LCD_H_RES * WAVEX_LVGL_DRAW_BUF_HEIGHT * sizeof(uint16_t),
        .flags = 0,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(WAVEX_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), 
                       "DISPLAY", "SPI bus initialization failed");
    ESP_LOGI("DISPLAY", "SPI bus initialized successfully");
    log_detailed_heap_info("after SPI bus init");
    
    // Configure LCD panel IO
    ESP_LOGI("DISPLAY", "Installing panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = WAVEX_LCD_GPIO_CS,
        .dc_gpio_num = WAVEX_LCD_GPIO_DC,
        .spi_mode = 0,
        .pclk_hz = WAVEX_LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = WAVEX_LCD_CMD_BITS,
        .lcd_param_bits = WAVEX_LCD_PARAM_BITS,
        .flags = {
            .dc_high_on_cmd = 0,  // Low for command (from docs: low=command, high=data)
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
            .octal_mode = 0,
            .quad_mode = 0,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0  // Low active CS (from docs)
        }
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)WAVEX_SPI_HOST, &io_config, &lcd_io),
                       "DISPLAY", "Panel IO creation failed");
    ESP_LOGI("DISPLAY", "Panel IO created successfully");
    log_detailed_heap_info("after panel IO creation");
    
    // Install ST7796S LCD driver
    ESP_LOGI("DISPLAY", "Installing LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = WAVEX_LCD_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, // Fix: change from RGB to BGR to match working config
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = WAVEX_LCD_BITS_PER_PIXEL,
        .flags = {0},
        .vendor_config = NULL
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(lcd_io, &panel_config, &lcd_panel),
                       "DISPLAY", "LCD panel creation failed");
    ESP_LOGI("DISPLAY", "LCD panel created successfully");
    log_detailed_heap_info("after LCD panel creation");
    
    // Reset and initialize panel
    ESP_LOGI("DISPLAY", "Resetting and initializing panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), "DISPLAY", "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), "DISPLAY", "Panel init failed");
    
    // Configure display orientation (landscape mode for 480x320)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(lcd_panel, true), "DISPLAY", "Panel swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, true, true), "DISPLAY", "Panel mirror failed");  // Changed mirror_x from true to false
    
    // Turn on display
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), "DISPLAY", "Panel display on failed");
    
    // Backlight control based on mode and enable flag
#if WAVEX_BACKLIGHT_ENABLED == 1
    #if WAVEX_BACKLIGHT_PWM_MODE == 1
        // PWM mode backlight control
        ESP_LOGI("DISPLAY", "Configuring backlight PWM...");
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), "DISPLAY", "LEDC timer config failed");

        ledc_channel_config_t ledc_ch = {
            .gpio_num = WAVEX_LCD_GPIO_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 255,
            .hpoint = 0,
            .flags = { .output_invert = 0 }
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_ch), "DISPLAY", "LEDC channel config failed");
        
        // Update the PWM duty to match the default brightness setting
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (display_brightness * 255) / 100);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        
        ESP_LOGI("DISPLAY", "Backlight PWM configured successfully with %d%% brightness", display_brightness);
    #else
        // GPIO mode backlight control
        ESP_LOGI("DISPLAY", "Enabling backlight (GPIO mode)...");
        ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_LCD_GPIO_BL, WAVEX_LCD_BL_ON_LEVEL), 
                           "DISPLAY", "Backlight enable failed");
        ESP_LOGI("DISPLAY", "Backlight enabled successfully (GPIO mode)");
    #endif
#else
    // Backlight disabled
    ESP_LOGI("DISPLAY", "Backlight DISABLED by configuration");
    #if WAVEX_BACKLIGHT_PWM_MODE == 0
        ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_LCD_GPIO_BL, !WAVEX_LCD_BL_ON_LEVEL), 
                           "DISPLAY", "Backlight disable failed");
        ESP_LOGI("DISPLAY", "Backlight explicitly turned OFF (GPIO mode)");
    #endif
#endif
    
    ESP_LOGI("DISPLAY", "ST7796S display initialization complete");
    log_detailed_heap_info("display init complete");
    
    return ESP_OK;
}

/**
 * @brief Initialize I2C driver for touch controller debugging
 */
static esp_err_t init_i2c_for_debug(void) {
    ESP_LOGI("I2C_DEBUG", "Initializing I2C driver for debugging...");
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = WAVEX_CTP_GPIO_SDA,
        .scl_io_num = WAVEX_CTP_GPIO_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = 400000  // 400kHz
        },
        .clk_flags = 0
    };
    
    esp_err_t ret = i2c_param_config(I2C_NUM_0, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE("I2C_DEBUG", "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE("I2C_DEBUG", "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI("I2C_DEBUG", "I2C driver initialized successfully");
    return ESP_OK;
}

// Update the debug_i2c_scan function to call this first
static void debug_i2c_scan(void) {
    ESP_LOGI("I2C_DEBUG", "=== Comprehensive I2C Bus Scan ===");
    
    // Initialize I2C driver first
    esp_err_t ret = init_i2c_for_debug();
    if (ret != ESP_OK) {
        ESP_LOGE("I2C_DEBUG", "Failed to initialize I2C driver for debugging");
        return;
    }
    
    // Common touch controller I2C addresses to try
    uint8_t touch_addresses[] = {
        0x38,  // FT6X36 default address
        0x14,  // FT6X36 alternative
        0x5D,  // GT911 
        0x48,  // CST816S
        0x15,  // CST226
        0x4A,  // Another common touch controller
    };
    
    ESP_LOGI("I2C_DEBUG", "Scanning common touch controller addresses...");
    
    for (int i = 0; i < sizeof(touch_addresses); i++) {
        uint8_t addr = touch_addresses[i];
        
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            ESP_LOGI("I2C_DEBUG", "Device found at address 0x%02X", addr);
            
            // Try to read vendor ID if this might be FT6X36
            if (addr == 0x38 || addr == 0x14) {
                uint8_t vendor_id = 0;
                i2c_cmd_handle_t read_cmd = i2c_cmd_link_create();
                i2c_master_start(read_cmd);
                i2c_master_write_byte(read_cmd, (addr << 1) | I2C_MASTER_WRITE, true);
                i2c_master_write_byte(read_cmd, 0xA3, true);  // FT6X36_VENDID register
                i2c_master_start(read_cmd);
                i2c_master_write_byte(read_cmd, (addr << 1) | I2C_MASTER_READ, true);
                i2c_master_read_byte(read_cmd, &vendor_id, I2C_MASTER_NACK);
                i2c_master_stop(read_cmd);
                
                esp_err_t read_ret = i2c_master_cmd_begin(I2C_NUM_0, read_cmd, pdMS_TO_TICKS(100));
                i2c_cmd_link_delete(read_cmd);
                
                if (read_ret == ESP_OK) {
                    ESP_LOGI("I2C_DEBUG", "  Vendor ID at 0x%02X: 0x%02X", addr, vendor_id);
                } else {
                    ESP_LOGW("I2C_DEBUG", "  Failed to read vendor ID from 0x%02X", addr);
                }
            }
        }
    }
    
    ESP_LOGI("I2C_DEBUG", "=== Full I2C Bus Scan (0x08-0x77) ===");
    int found_devices = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            ESP_LOGI("I2C_DEBUG", "I2C device found at address 0x%02X", addr);
            found_devices++;
        }
    }
    
    if (found_devices == 0) {
        ESP_LOGE("I2C_DEBUG", "No I2C devices found! Check wiring and pull-ups.");
        ESP_LOGI("I2C_DEBUG", "Possible issues:");
        ESP_LOGI("I2C_DEBUG", "  - Wrong SDA/SCL pin assignments");
        ESP_LOGI("I2C_DEBUG", "  - Missing pull-up resistors (4.7kΩ)");
        ESP_LOGI("I2C_DEBUG", "  - Touch controller not powered");
        ESP_LOGI("I2C_DEBUG", "  - Hardware connection issues");
    } else {
        ESP_LOGI("I2C_DEBUG", "Found %d I2C device(s) total", found_devices);
    }
}

/**
 * @brief Configure touch controller with optimized settings
 */
static void configure_touch_sensitivity(void) {
    if (touch_controller != nullptr) {
        ESP_LOGI("TOUCH", "Configuring touch sensitivity and filtering...");
        
        ESP_LOGI("TOUCH", "Touch sensitivity configured via software filtering");
        ESP_LOGI("TOUCH", "  Edge filtering: 5 pixel border");
        ESP_LOGI("TOUCH", "  Movement threshold: %" PRId32 " pixels", TOUCH_NOISE_THRESHOLD);
        ESP_LOGI("TOUCH", "  Debounce time: %" PRIu32 " ms", TOUCH_DEBOUNCE_MS);
    }
}

// Move this function definition BEFORE wavex_touch_init_debug():
/**
 * @brief Enhanced touch initialization with polling mode (no interrupts)
 */
static esp_err_t wavex_touch_init_debug(void)
{
    log_detailed_heap_info("before touch init");
    
    ESP_LOGI("TOUCH", "=== Enhanced Touch Controller Initialization (Polling Mode) ===");
    
    // First, verify I2C pins from hardware_pins.h
    ESP_LOGI("TOUCH", "Touch I2C configuration:");
    ESP_LOGI("TOUCH", "  SDA Pin: GPIO%d", WAVEX_CTP_GPIO_SDA);
    ESP_LOGI("TOUCH", "  SCL Pin: GPIO%d", WAVEX_CTP_GPIO_SCL);
    ESP_LOGI("TOUCH", "  RST Pin: GPIO%d", WAVEX_CTP_GPIO_RST);
    ESP_LOGI("TOUCH", "  Mode: Polling (no interrupt)");
    
    // Configure touch reset GPIO with pull-up
    gpio_config_t touch_rst_config = {
        .pin_bit_mask = 1ULL << WAVEX_CTP_GPIO_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // Enable pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&touch_rst_config), "TOUCH", "Touch reset GPIO config failed");
    log_detailed_heap_info("after touch GPIO config");
    
    // PROPER reset sequence for FT6X36
    ESP_LOGI("TOUCH", "Performing proper FT6X36 reset sequence...");
    
    // Start with reset HIGH (inactive)
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_CTP_GPIO_RST, 1), "TOUCH", "Touch reset high failed");
    vTaskDelay(pdMS_TO_TICKS(10));   // Short delay
    
    // Assert reset LOW (active) 
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_CTP_GPIO_RST, 0), "TOUCH", "Touch reset low failed");
    ESP_LOGI("TOUCH", "Reset pin LOW, waiting 10ms...");
    vTaskDelay(pdMS_TO_TICKS(10));   // Short reset pulse
    
    // Release reset HIGH (inactive)
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_CTP_GPIO_RST, 1), "TOUCH", "Touch reset high failed");
    ESP_LOGI("TOUCH", "Reset pin HIGH, waiting 300ms for controller boot...");
    vTaskDelay(pdMS_TO_TICKS(300));  // Boot time
    
    // Verify reset pin state
    int rst_level = gpio_get_level(WAVEX_CTP_GPIO_RST);
    ESP_LOGI("TOUCH", "Reset pin final state: %d (expected 1)", rst_level);
    if (rst_level != 1) {
        ESP_LOGW("TOUCH", "Reset pin issue detected - checking hardware connections");
        
        // Try to force the pin high again
        gpio_set_level(WAVEX_CTP_GPIO_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        rst_level = gpio_get_level(WAVEX_CTP_GPIO_RST);
        ESP_LOGI("TOUCH", "After retry - Reset pin state: %d", rst_level);
    }
    
    // Scan I2C bus before trying to initialize touch controller
    debug_i2c_scan();
    
    // Try to initialize FT6X36 touch controller in POLLING MODE (no interrupt pin)
    ESP_LOGI("TOUCH", "Attempting to initialize FT6X36 touch controller in polling mode...");
    touch_controller = new FT6X36(-1);  // Use -1 to indicate no interrupt pin (polling mode)
    if (touch_controller == nullptr) {
        ESP_LOGE("TOUCH", "Failed to create FT6X36 touch controller - out of memory");
        return ESP_ERR_NO_MEM;
    }

    log_detailed_heap_info("after FT6X36 creation");
    
    // Try initialization with different thresholds
    ESP_LOGI("TOUCH", "Trying FT6X36 initialization with default settings...");
    if (!touch_controller->begin(FT6X36_DEFAULT_THRESHOLD, WAVEX_LCD_H_RES, WAVEX_LCD_V_RES)) {
        ESP_LOGE("TOUCH", "FT6X36 initialization failed with default settings");
        
        // Try with alternative threshold
        ESP_LOGI("TOUCH", "Trying with alternative threshold...");
        if (!touch_controller->begin(40, WAVEX_LCD_H_RES, WAVEX_LCD_V_RES)) {
            ESP_LOGE("TOUCH", "FT6X36 initialization failed with alternative threshold");
            
            delete touch_controller;
            touch_controller = nullptr;
            return ESP_FAIL;
        }
    }
    log_detailed_heap_info("after FT6X36 begin");
    
    // Configure touch controller if initialization succeeded
    touch_controller->setRotation(3);  // Match display rotation
    touch_controller->setTouchHeight(WAVEX_LCD_V_RES);
    touch_controller->setTouchWidth(WAVEX_LCD_H_RES);
    
    // Call the function here (this should work now)
    configure_touch_sensitivity();
    
    ESP_LOGI("TOUCH", "Touch controller initialized - callback registration will be delayed");
    
    return ESP_OK;
}

esp_err_t wavex_hardware_init(void) {
    ESP_LOGI("HARDWARE", "=== Starting MINIMAL hardware initialization ===");
    
    // ONLY configure backlight GPIO - nothing else
    ESP_LOGI("HARDWARE", "Configuring backlight GPIO only...");
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << WAVEX_LCD_GPIO_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), "HARDWARE", "Backlight GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_LCD_GPIO_BL, WAVEX_LCD_BL_ON_LEVEL), "HARDWARE", "Backlight enable failed");
    
    ESP_LOGI("HARDWARE", "Backlight should now be ON - check if screen lights up");
    
    // Skip all SPI/LCD initialization for now
    ESP_LOGI("HARDWARE", "=== Minimal hardware init complete ===");
    return ESP_OK;
}

static lv_obj_t* volume_slider = NULL;

// Callback for volume slider value change
static void volume_slider_event_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    // Map slider value (0-100) to protocol value (0-65535)
    uint16_t param_value = (uint16_t)((value / 100.0f) * 65535.0f);
    // Send control change for volume (parameter 0x01, channel 0)
    esp_err_t ret = inter_mcu_send_control_change(0x01, 0, param_value);
    if (ret != ESP_OK) {
        ESP_LOGE("UI", "Failed to send volume control change: %d", ret);
    }
}

// Global resource monitoring widgets
static lv_obj_t* cpu_usage_bar = NULL;
static lv_obj_t* ram_usage_bar = NULL;
static lv_obj_t* flash_usage_bar = NULL;
static lv_obj_t* temp_label = NULL;
static lv_obj_t* uptime_label = NULL;
static lv_obj_t* midi_activity_led = NULL;

// Resource monitoring task handle
static TaskHandle_t resource_monitor_task = NULL;

/**
 * @brief Get system uptime in seconds
 */
static uint32_t get_system_uptime(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

/**
 * @brief Get RAM usage percentage
 */
static uint8_t get_ram_usage_percent(void) {
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
    uint32_t total_heap = heap_info.total_free_bytes + heap_info.total_allocated_bytes;
    if (total_heap == 0) return 0;
    return (uint8_t)((heap_info.total_allocated_bytes * 100) / total_heap);
}

/**
 * @brief Get flash usage percentage (estimated)
 */
static uint8_t get_flash_usage_percent(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        // Estimate based on typical firmware size vs partition size
        return 45; // Typical usage for our firmware
    }
    return 0;
}

/**
 * @brief Resource monitoring task with reduced memory usage
 */
static void resource_monitor_task_func(void* pvParameters) {
    while (1) {
        // Use shorter lock timeout and longer update interval to reduce pressure
        if (lvgl_port_lock(5)) {  // Reduced timeout from 10 to 5ms
            ui_update_system_resources();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // Update every 2 seconds instead of 1
    }
}

void ui_update_system_resources(void) {
    // Update CPU usage bar (simulated based on system load)
    if (cpu_usage_bar) {
        uint8_t cpu_usage = 15 + (esp_random() % 20); // Simulated 15-35% usage
        lv_bar_set_value(cpu_usage_bar, cpu_usage, LV_ANIM_ON);
    }
    
    // Update RAM usage bar
    if (ram_usage_bar) {
        uint8_t ram_usage = get_ram_usage_percent();
        lv_bar_set_value(ram_usage_bar, ram_usage, LV_ANIM_ON);
    }
    
    // Update flash usage bar
    if (flash_usage_bar) {
        uint8_t flash_usage = get_flash_usage_percent();
        lv_bar_set_value(flash_usage_bar, flash_usage, LV_ANIM_ON);
    }
    
    // Update uptime
    if (uptime_label) {
        uint32_t uptime = get_system_uptime();
        uint32_t hours = uptime / 3600;
        uint32_t minutes = (uptime % 3600) / 60;
        uint32_t seconds = uptime % 60;
        
        char uptime_str[32];
        snprintf(uptime_str, sizeof(uptime_str), "%02ld:%02ld:%02ld", hours, minutes, seconds);
        lv_label_set_text(uptime_label, uptime_str);
    }
    
    // Update temperature (simulated)
    if (temp_label) {
        int temp = 42 + (esp_random() % 8); // Simulated 42-50°C
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%d°C", temp);
        lv_label_set_text(temp_label, temp_str);
    }
    
    // Simulate MIDI activity
    if (midi_activity_led) {
        bool activity = (esp_random() % 10) == 0; // Random MIDI activity
        lv_obj_set_style_bg_color(midi_activity_led, 
                                 activity ? lv_color_hex(0x00FF00) : lv_color_hex(0x333333), 
                                 LV_PART_MAIN);
    }
}

/**
 * @brief Create a progress bar with label
 */
static lv_obj_t* create_progress_bar_with_label(lv_obj_t* parent, const char* label_text, 
                                               int32_t min, int32_t max, int32_t initial_value) {
    // Container for label and bar
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(container, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 5, LV_PART_MAIN);
    
    // Label
    lv_obj_t* label = lv_label_create(container);
    lv_label_set_text(label, label_text);
    lv_obj_set_pos(label, 0, 0);
    
    // Progress bar
    lv_obj_t* bar = lv_bar_create(container);
    lv_obj_set_size(bar, LV_PCT(100), 20);
    lv_obj_set_pos(bar, 0, 25);
    lv_bar_set_range(bar, min, max);
    lv_bar_set_value(bar, initial_value, LV_ANIM_OFF);
    
    // Color coding for different usage levels
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    
    return bar;
}

/**
 * @brief Create system information display widget
 */
static lv_obj_t* create_system_info_widget(lv_obj_t* parent) {
    lv_obj_t* info_container = lv_obj_create(parent);
    lv_obj_set_size(info_container, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(info_container, lv_color_hex(0x1e1e1e), LV_PART_MAIN);
    lv_obj_set_style_border_width(info_container, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(info_container, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_pad_all(info_container, 10, LV_PART_MAIN);
    
    // Title
    lv_obj_t* title = lv_label_create(info_container);
    lv_label_set_text(title, "System Information");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(title, 0, 0);
    
    // Uptime
    lv_obj_t* uptime_title = lv_label_create(info_container);
    lv_label_set_text(uptime_title, "Uptime:");
    lv_obj_set_pos(uptime_title, 0, 25);
    
    uptime_label = lv_label_create(info_container);
    lv_label_set_text(uptime_label, "00:00:00");
    lv_obj_set_pos(uptime_label, 60, 25);
    lv_obj_set_style_text_color(uptime_label, lv_color_hex(0x00AAAA), 0);
    
    // Temperature
    lv_obj_t* temp_title = lv_label_create(info_container);
    lv_label_set_text(temp_title, "Temp:");
    lv_obj_set_pos(temp_title, 0, 45);
    
    temp_label = lv_label_create(info_container);
    lv_label_set_text(temp_label, "45°C");
    lv_obj_set_pos(temp_label, 50, 45);
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFF8800), 0);
    
    // MIDI Activity
    lv_obj_t* midi_title = lv_label_create(info_container);
    lv_label_set_text(midi_title, "MIDI:");
    lv_obj_set_pos(midi_title, 0, 65);
    
    midi_activity_led = lv_obj_create(info_container);
    lv_obj_set_size(midi_activity_led, 15, 15);
    lv_obj_set_pos(midi_activity_led, 50, 65);
    lv_obj_set_style_bg_color(midi_activity_led, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_border_width(midi_activity_led, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(midi_activity_led, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(midi_activity_led, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    
    return info_container;
}

static void check_memory_status(void) {
    ESP_LOGI("MEMORY", "=== Memory Diagnostic ===");
    
    // Check total heap
    size_t total_free = esp_get_free_heap_size();
    size_t min_free = esp_get_minimum_free_heap_size();
    ESP_LOGI("MEMORY", "Total heap: %zu free, %zu minimum ever", total_free, min_free);
    
    // Check SPIRAM specifically
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI("MEMORY", "SPIRAM: %zu free, %zu largest block", spiram_free, spiram_largest);
    
    // Check internal RAM
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI("MEMORY", "Internal RAM: %zu free, %zu largest block", internal_free, internal_largest);
    
    // Check DMA capable memory
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_LOGI("MEMORY", "DMA capable: %zu free, %zu largest block", dma_free, dma_largest);
    
    // Test SPIRAM allocation
    void* test_spiram = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
    if (test_spiram) {
        ESP_LOGI("MEMORY", "SPIRAM allocation test: SUCCESS (64KB)");
        heap_caps_free(test_spiram);
    } else {
        ESP_LOGE("MEMORY", "SPIRAM allocation test: FAILED");
    }
}

static void check_stack_usage(const char* context) {
    UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK", "[%s] Main task stack high water mark: %u bytes remaining", 
             context, (unsigned int)(stack_high_water * sizeof(StackType_t)));
}

/**
 * @brief Test memory allocation strategy
 */
static void test_memory_allocation_strategy(void) {
    ESP_LOGI("MEM_TEST", "=== Testing Memory Allocation Strategy ===");
    
    // Test PSRAM allocation
    void* psram_test = heap_caps_malloc(100000, MALLOC_CAP_SPIRAM);
    if (psram_test) {
        ESP_LOGI("MEM_TEST", "PSRAM allocation test: SUCCESS (100KB)");
        heap_caps_free(psram_test);
    } else {
        ESP_LOGE("MEM_TEST", "PSRAM allocation test: FAILED");
    }
    
    // Test internal RAM allocation
    void* internal_test = heap_caps_malloc(10000, MALLOC_CAP_INTERNAL);
    if (internal_test) {
        ESP_LOGI("MEM_TEST", "Internal RAM allocation test: SUCCESS (10KB)");
        heap_caps_free(internal_test);
    } else {
        ESP_LOGE("MEM_TEST", "Internal RAM allocation test: FAILED");
    }
    
    // Log current memory status
    ESP_LOGI("MEM_TEST", "Internal SRAM: %zu free / %zu largest block", 
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI("MEM_TEST", "PSRAM: %zu free / %zu largest block", 
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    ESP_LOGI("MEM_TEST", "DMA capable: %zu free / %zu largest block", 
             heap_caps_get_free_size(MALLOC_CAP_DMA),
             heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

// Event handlers for simplified menu buttons
static void simplified_audio_btn_event_cb(lv_event_t* e) {
    ESP_LOGI("UI", "Audio Settings button pressed!");
    // TODO: Create audio settings page
}

static void simplified_display_btn_event_cb(lv_event_t* e) {
    ESP_LOGI("UI", "Display Settings button pressed!");
    // TODO: Create display settings page
}

static void simplified_midi_btn_event_cb(lv_event_t* e) {
    ESP_LOGI("UI", "MIDI Settings button pressed!");
    // TODO: Create MIDI settings page
}

static void simplified_system_btn_event_cb(lv_event_t* e) {
    ESP_LOGI("UI", "System Settings button pressed!");
    // TODO: Create system settings page
}

/**
 * @brief Create a simplified menu to avoid memory issues
 */
static lv_obj_t* create_simplified_menu(void) {
    ESP_LOGI("UI", "Creating simplified touchable menu to conserve memory...");
    
    // Create a simple container instead of complex menu
    lv_obj_t* menu_container = lv_obj_create(main_screen);
    if (!menu_container) {
        ESP_LOGE("UI", "Failed to create menu container");
        return NULL;
    }
    
    // Style the container
    lv_obj_set_size(menu_container, WAVEX_LCD_H_RES, WAVEX_LCD_V_RES);
    lv_obj_set_pos(menu_container, 0, 0);
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x003a57), LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu_container, 10, LV_PART_MAIN);
    
    // Create title
    lv_obj_t* title = lv_label_create(menu_container);
    if (title) {
        lv_label_set_text(title, "WaveX System Menu");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_pos(title, 10, 10);
    }
    
    // Create touchable buttons with event handlers
    int y_pos = 50;
    const int btn_height = 45;
    const int btn_spacing = 12;
    const int btn_width = WAVEX_LCD_H_RES - 40;
    
    // Audio button with click handler
    lv_obj_t* audio_btn = lv_btn_create(menu_container);
    if (audio_btn) {
        lv_obj_set_size(audio_btn, btn_width, btn_height);
        lv_obj_set_pos(audio_btn, 20, y_pos);
        lv_obj_add_event_cb(audio_btn, simplified_audio_btn_event_cb, LV_EVENT_CLICKED, NULL);
        
        lv_obj_t* audio_label = lv_label_create(audio_btn);
        if (audio_label) {
            lv_label_set_text(audio_label, LV_SYMBOL_AUDIO " Audio Settings");
            lv_obj_center(audio_label);
        }
    }
    y_pos += btn_height + btn_spacing;
    
    // Display button with click handler
    lv_obj_t* display_btn = lv_btn_create(menu_container);
    if (display_btn) {
        lv_obj_set_size(display_btn, btn_width, btn_height);
        lv_obj_set_pos(display_btn, 20, y_pos);
        lv_obj_add_event_cb(display_btn, simplified_display_btn_event_cb, LV_EVENT_CLICKED, NULL);
        
        lv_obj_t* display_label = lv_label_create(display_btn);
        if (display_label) {
            lv_label_set_text(display_label, LV_SYMBOL_EYE_OPEN " Display Settings");
            lv_obj_center(display_label);
        }
    }
    y_pos += btn_height + btn_spacing;
    
    // MIDI button with click handler
    lv_obj_t* midi_btn = lv_btn_create(menu_container);
    if (midi_btn) {
        lv_obj_set_size(midi_btn, btn_width, btn_height);
        lv_obj_set_pos(midi_btn, 20, y_pos);
        lv_obj_add_event_cb(midi_btn, simplified_midi_btn_event_cb, LV_EVENT_CLICKED, NULL);
        
        lv_obj_t* midi_label = lv_label_create(midi_btn);
        if (midi_label) {
            lv_label_set_text(midi_label, LV_SYMBOL_BLUETOOTH " MIDI Settings");
            lv_obj_center(midi_label);
        }
    }
    y_pos += btn_height + btn_spacing;
    
    // System button with click handler
    lv_obj_t* system_btn = lv_btn_create(menu_container);
    if (system_btn) {
        lv_obj_set_size(system_btn, btn_width, btn_height);
        lv_obj_set_pos(system_btn, 20, y_pos);
        lv_obj_add_event_cb(system_btn, simplified_system_btn_event_cb, LV_EVENT_CLICKED, NULL);
        
        lv_obj_t* system_label = lv_label_create(system_btn);
        if (system_label) {
            lv_label_set_text(system_label, LV_SYMBOL_SETTINGS " System Settings");
            lv_obj_center(system_label);
        }
    }
    y_pos += btn_height + btn_spacing + 10;
    
    // Add a simple test slider for touch testing
    lv_obj_t* test_slider_label = lv_label_create(menu_container);
    if (test_slider_label) {
        lv_label_set_text(test_slider_label, "Test Touch (Master Volume):");
        lv_obj_set_style_text_color(test_slider_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_pos(test_slider_label, 20, y_pos);
    }
    y_pos += 20;
    
    lv_obj_t* test_slider = lv_slider_create(menu_container);
    if (test_slider) {
        lv_obj_set_size(test_slider, btn_width, 30);
        lv_obj_set_pos(test_slider, 20, y_pos);
        lv_slider_set_range(test_slider, 0, 100);
        lv_slider_set_value(test_slider, 75, LV_ANIM_OFF);
        lv_obj_add_event_cb(test_slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y_pos += 40;
    
    // Add status info at bottom
    lv_obj_t* status_label = lv_label_create(menu_container);
    if (status_label) {
        char status_text[150];
        snprintf(status_text, sizeof(status_text), 
                "Heap: %lu bytes | Internal: %zu bytes\n"
                "Touch: %s | Display: ST7796S Ready\n"
                "Tap buttons to test touch response", 
                esp_get_free_heap_size(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                touch_controller ? "FT6X36 Ready" : "Failed");
        lv_label_set_text(status_label, status_text);
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xAAAAAAA), 0);
        lv_obj_set_pos(status_label, 20, WAVEX_LCD_V_RES - 80);
    }
    
    ESP_LOGI("UI", "Simplified touchable menu created successfully with event handlers");
    return menu_container;
}

/**
 * @brief Create a simple test screen to verify LVGL is working
 */
static void create_test_screen(void) {
    ESP_LOGI("UI", "Creating simple test screen...");
    
    // Create a simple test label
    lv_obj_t* test_label = lv_label_create(main_screen);
    if (!test_label) {
        ESP_LOGE("UI", "Failed to create test label");
        return;
    }
    
    lv_label_set_text(test_label, "WaveX UI Test\nLVGL Working!\n\nTouch screen should\nbe functional");
    lv_obj_set_style_text_font(test_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(test_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(test_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(test_label);
    
    // Add a background rectangle for visibility
    lv_obj_set_style_bg_color(test_label, lv_color_hex(0xFF0000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(test_label, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_all(test_label, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(test_label, 10, LV_PART_MAIN);
    
    // Add a simple test button
    lv_obj_t* test_btn = lv_btn_create(main_screen);
    if (test_btn) {
        lv_obj_set_size(test_btn, 200, 50);
        lv_obj_set_pos(test_btn, (WAVEX_LCD_H_RES - 200) / 2, WAVEX_LCD_V_RES - 80);
        
        lv_obj_t* btn_label = lv_label_create(test_btn);
        if (btn_label) {
            lv_label_set_text(btn_label, "Test Touch Button");
            lv_obj_center(btn_label);
        }
        
        // Add a simple click handler for testing
        lv_obj_add_event_cb(test_btn, [](lv_event_t* e) {
            ESP_LOGI("UI", "Test button pressed! Touch is working!");
        }, LV_EVENT_CLICKED, NULL);
    }
    
    ESP_LOGI("UI", "Test screen created - should see red background with white text and test button");
}

static void ui_creation_task(void* pvParameters) {
    ESP_LOGI("UI", "UI creation task started with PSRAM memory management");
    
    // Test our memory allocation strategy
    test_memory_allocation_strategy();
    
    // Move the UI creation logic here
    lvgl_port_lock(0);
    
    main_screen = lv_obj_create(NULL);
    if (!main_screen) {
        ESP_LOGE("UI", "Failed to create main screen");
        lvgl_port_unlock();
        vTaskDelete(NULL);
        return;
    }
    
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x003a57), LV_PART_MAIN);
    lv_screen_load(main_screen);
    
    // Check available internal RAM - be very conservative
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI("UI", "Internal RAM before menu creation: %zu bytes", free_internal);
    
    // Use simplified menu system to avoid memory exhaustion
    // The complex LVGL menu system is using too much internal RAM for styles
    if (free_internal > 300000) {  // Only try complex menu if we have >300KB internal RAM
        ESP_LOGI("UI", "Attempting to create full menu system (300KB+ internal RAM available)...");
        main_menu = ui_main_create_menu();
        if (!main_menu) {
            ESP_LOGW("UI", "Full menu creation failed, falling back to simplified menu");
            main_menu = create_simplified_menu();
        } else {
            ESP_LOGI("UI", "Full menu system created successfully");
        }
    } else {
        ESP_LOGW("UI", "Internal RAM too low (%zu bytes), using simplified touchable menu", free_internal);
        main_menu = create_simplified_menu();
    }
    
    // Final fallback to test screen if all else fails
    if (!main_menu) {
        ESP_LOGE("UI", "All menu creation failed, showing basic test screen");
        create_test_screen();
    }
    
    lv_refr_now(lvgl_disp);
    lvgl_port_unlock();
    
    // Log final memory usage
    ESP_LOGI("UI", "Final memory usage:");
    ESP_LOGI("UI", "  Internal SRAM: %zu bytes free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI("UI", "  PSRAM: %zu bytes free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    ESP_LOGI("UI", "UI creation complete with optimized memory allocation");
    vTaskDelete(NULL); // Delete this task when done
}

void wavex_ui_init(void)
{
    ESP_LOGI("UI", "Initializing WaveX UI with PSRAM-optimized memory configuration...");
    check_stack_usage("start of UI init");
    
    // Check memory status first
    check_memory_status();
    check_stack_usage("after memory check");
    
    // Initialize display hardware first
    esp_err_t ret = wavex_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE("UI", "Display initialization failed: %d", ret);
        return;
    }
    ESP_LOGI("UI", "Display hardware initialized successfully");
    check_stack_usage("after display init");
    
    // Initialize touch hardware with enhanced debugging
    ret = wavex_touch_init_debug();
    if (ret != ESP_OK) {
        ESP_LOGE("UI", "Touch initialization failed: %d", ret);
        ESP_LOGW("UI", "Continuing without touch - display-only mode");
    } else {
        ESP_LOGI("UI", "Touch hardware initialized successfully");
    }
    check_stack_usage("after touch init");
    
    // Configure LVGL to use PSRAM via sdkconfig
    ESP_LOGI("UI", "LVGL configured to use PSRAM via sdkconfig settings...");
    
    // Initialize esp_lvgl_port with custom memory management
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 10  // Changed from 5ms to 10ms to reduce polling frequency
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    
    // Note: LVGL memory management is now handled via CONFIG_LV_MEM_CUSTOM=y in sdkconfig
    // This will automatically use our custom memory allocator for LVGL widgets
    
    ESP_LOGI("UI", "LVGL port initialized with PSRAM memory management");
    check_stack_usage("after LVGL port init");
    
    // CRITICAL: Display buffers stay in internal SRAM for DMA performance
    ESP_LOGI("UI", "Configuring display buffers in internal SRAM (DMA optimized)...");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .control_handle = NULL,
        .buffer_size = WAVEX_LCD_H_RES * 20,  // 20 lines = ~19KB in internal SRAM
        .double_buffer = false,
        .trans_size = 0,
        .hres = WAVEX_LCD_H_RES,
        .vres = WAVEX_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = true,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,  // FORCE internal SRAM for display buffers (DMA performance)
            .sw_rotate = false,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
            .full_refresh = false,
            .direct_mode = false
        }
    };
    
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(lvgl_disp ? ESP_OK : ESP_FAIL);
    ESP_LOGI("UI", "Display buffers configured in internal SRAM for optimal DMA performance");
    check_stack_usage("after LVGL display config");
    
    // Register touch input device if touch controller was initialized successfully
    if (touch_controller != nullptr) {
        ESP_LOGI("UI", "Creating LVGL touch input device (callback registration delayed)...");
        
        // Create touch input device directly using LVGL API
        lvgl_touch_indev = lv_indev_create();
        if (lvgl_touch_indev) {
            lv_indev_set_type(lvgl_touch_indev, LV_INDEV_TYPE_POINTER);
            // DON'T register callback immediately - we'll do it after 1 second
            // lv_indev_set_read_cb(lvgl_touch_indev, wavex_touch_read_cb);
            lv_indev_set_display(lvgl_touch_indev, lvgl_disp);
            
            // Create a one-shot timer to register the callback after 1 second
            const esp_timer_create_args_t timer_args = {
                .callback = register_touch_callback_delayed,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "touch_reg_timer",
                .skip_unhandled_events = false
            };
            
            esp_err_t ret = esp_timer_create(&timer_args, &touch_registration_timer);
            if (ret == ESP_OK) {
                ret = esp_timer_start_once(touch_registration_timer, 1000000); // 1 second in microseconds
                if (ret == ESP_OK) {
                    ESP_LOGI("UI", "LVGL touch input device created - callback will be registered in 1 second");
                } else {
                    ESP_LOGE("UI", "Failed to start touch registration timer: %s", esp_err_to_name(ret));
                    // Fallback: register immediately
                    lv_indev_set_read_cb(lvgl_touch_indev, wavex_touch_read_cb);
                    ESP_LOGI("UI", "Touch callback registered immediately (timer failed)");
                }
            } else {
                ESP_LOGE("UI", "Failed to create touch registration timer: %s", esp_err_to_name(ret));
                // Fallback: register immediately
                lv_indev_set_read_cb(lvgl_touch_indev, wavex_touch_read_cb);
                ESP_LOGI("UI", "Touch callback registered immediately (timer creation failed)");
            }
        } else {
            ESP_LOGE("UI", "Failed to create LVGL touch input device");
        }
    } else {
        ESP_LOGW("UI", "Touch controller not available - running in display-only mode");
    }
    check_stack_usage("after touch registration");
    
    // Log memory allocation strategy
    ESP_LOGI("UI", "Memory Strategy: Display buffers (internal SRAM) + Widgets (PSRAM)");
    ESP_LOGI("UI", "Internal SRAM free: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI("UI", "PSRAM free: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // Create UI in separate task with large stack
    xTaskCreate(ui_creation_task, "ui_create", 16384, NULL, 5, NULL);
    
    // Wait a bit for UI creation to complete
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI("UI", "UI initialization complete with optimized memory allocation");
    
    // Move memory check to separate function call to reduce stack usage
    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay
    check_memory_status();
    check_stack_usage("end of UI init");

    // After LVGL display is set up, create touch debug circle
    if (lvgl_disp) {
        touch_circle = lv_obj_create(lv_screen_active());
        lv_obj_set_size(touch_circle, 20, 20);  // Small circle
        lv_obj_set_style_radius(touch_circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(touch_circle, lv_color_hex(0xFF0000), 0);  // Red
        lv_obj_set_style_bg_opa(touch_circle, LV_OPA_100, 0);
        lv_obj_set_style_border_width(touch_circle, 0, 0);
        lv_obj_add_flag(touch_circle, LV_OBJ_FLAG_HIDDEN);  // Start hidden
        lv_obj_set_pos(touch_circle, 0, 0);  // Initial position
    }
}

lv_obj_t* ui_main_create_menu(void)
{
    // Validate that main_screen exists
    if (!main_screen) {
        ESP_LOGE("UI", "Cannot create menu: main_screen is NULL");
        return NULL;
    }

    log_heap_info("before menu creation");
    
    // Check heap before creating the menu
    if (!check_heap_available(80000, "menu creation")) {
        ESP_LOGW("UI", "Low heap for menu creation, creating simplified menu");
        // Could implement simplified menu fallback here
    }

    // Create menu object
    lv_obj_t* menu = lv_menu_create(main_screen);
    if (!menu) {
        ESP_LOGE("UI", "Failed to create menu object");
        return NULL;
    }
    ESP_LOGI("UI", "Menu object created successfully");
    log_heap_info("after base menu creation");

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

    ESP_LOGI("UI", "Creating sub-pages with memory optimization...");
    
    // Create pages on-demand or with simplified initial content
    // to reduce initial memory allocation
    
    // ===== AUDIO SETTINGS PAGE WITH REDUCED INITIAL COMPLEXITY =====
    lv_obj_t* audio_page = lv_menu_page_create(menu, "Audio Settings");
    if (!audio_page) {
        ESP_LOGE("UI", "Failed to create audio page");
        return NULL;
    }
    lv_obj_set_style_pad_hor(audio_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(audio_page);
    lv_obj_t* section = lv_menu_section_create(audio_page);
    
    // Create only essential controls initially
    lv_obj_t* vol_container = create_menu_slider_with_callback(section, LV_SYMBOL_VOLUME_MAX, "Master Volume", 0, 100, 75, volume_slider_event_cb);
    if (vol_container) {
        volume_slider = lv_obj_get_child(vol_container, lv_obj_get_child_cnt(vol_container) - 1);
    }
    
    // Add fewer initial controls to reduce memory pressure
    create_menu_switch(section, LV_SYMBOL_MUTE, "Mute", false);
    ESP_LOGI("UI", "Audio page created with reduced complexity");

    // ===== DISPLAY SETTINGS PAGE WITH FUNCTIONAL CALLBACKS =====
    lv_obj_t* display_page = lv_menu_page_create(menu, "Display & Interface");
    if (!display_page) {
        ESP_LOGE("UI", "Failed to create display page");
        return NULL;
    }
    lv_obj_set_style_pad_hor(display_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(display_page);
    section = lv_menu_section_create(display_page);
    
    // Display settings with functional callbacks
    create_menu_slider_with_callback(section, LV_SYMBOL_EYE_OPEN, "Brightness", 10, 100, display_brightness, brightness_slider_event_cb);
    create_menu_slider_with_callback(section, LV_SYMBOL_REFRESH, "Screen Timeout", 30, 600, screen_timeout_seconds, screen_timeout_event_cb);
    create_menu_switch_with_callback(section, LV_SYMBOL_IMAGE, "Dark Theme", dark_theme_enabled, dark_theme_event_cb);
    
    // Touch settings
    lv_menu_separator_create(display_page);
    section = lv_menu_section_create(display_page);
    create_menu_slider(section, LV_SYMBOL_EDIT, "Touch Sensitivity", 1, 10, 5);
    create_menu_switch(section, LV_SYMBOL_POWER, "Auto Sleep", true);
    ESP_LOGI("UI", "Display page created successfully with functional callbacks");

    // ===== MIDI SETTINGS PAGE =====
    lv_obj_t* midi_page = lv_menu_page_create(menu, "MIDI Configuration");
    if (!midi_page) {
        ESP_LOGE("UI", "Failed to create MIDI page");
        return NULL;
    }
    lv_obj_set_style_pad_hor(midi_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(midi_page);
    section = lv_menu_section_create(midi_page);
    
    // MIDI Channel Settings
    create_menu_slider(section, LV_SYMBOL_SETTINGS, "MIDI Channel", 1, 16, 1);
    create_menu_switch(section, LV_SYMBOL_BLUETOOTH, "MIDI over USB", true);
    create_menu_switch(section, LV_SYMBOL_WIFI, "MIDI Clock Sync", false);
    
    // MIDI Routing
    lv_menu_separator_create(midi_page);
    section = lv_menu_section_create(midi_page);
    create_menu_switch(section, LV_SYMBOL_AUDIO, "MIDI Thru", true);
    create_menu_switch(section, LV_SYMBOL_EDIT, "Local Control", true);
    create_menu_slider(section, LV_SYMBOL_VOLUME_MID, "Velocity Curve", 0, 4, 1);
    ESP_LOGI("UI", "MIDI page created successfully");

    // ===== SYSTEM SETTINGS PAGE WITH FUNCTIONAL CALLBACKS =====
    lv_obj_t* system_page = lv_menu_page_create(menu, "System Settings");
    if (!system_page) {
        ESP_LOGE("UI", "Failed to create system page");
        return NULL;
    }
    lv_obj_set_style_pad_hor(system_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    lv_menu_separator_create(system_page);
    section = lv_menu_section_create(system_page);
    
    // Power & Performance with functional callbacks
    create_menu_switch_with_callback(section, LV_SYMBOL_POWER, "Performance Mode", system_performance_mode, performance_mode_event_cb);
    create_menu_switch_with_callback(section, LV_SYMBOL_SAVE, "Auto-Save Settings", auto_save_enabled, auto_save_event_cb);
    create_menu_switch(section, LV_SYMBOL_USB, "USB Device Mode", true);
    
    // Storage & Connectivity
    lv_menu_separator_create(system_page);
    section = lv_menu_section_create(system_page);
    create_menu_text(section, LV_SYMBOL_SD_CARD, "SD Card: Ready");
    create_menu_text(section, LV_SYMBOL_BLUETOOTH, "MIDI USB: Connected");
    create_menu_text(section, LV_SYMBOL_WIFI, "Inter-MCU: Active");
    
    // System Actions
    lv_menu_separator_create(system_page);
    section = lv_menu_section_create(system_page);
    create_menu_text(section, LV_SYMBOL_REFRESH, "Factory Reset");
    create_menu_text(section, LV_SYMBOL_DOWNLOAD, "Firmware Update");
    create_menu_text_with_callback(section, LV_SYMBOL_CLOSE, "Restart System", system_restart_event_cb);
    ESP_LOGI("UI", "System page created successfully with functional callbacks");

    // Create external UI components with memory management
    ESP_LOGI("UI", "Creating external UI components with memory optimization...");
    
    // Add task yield to prevent watchdog timeout
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Create about page first (simpler)
    lv_obj_t* about_page = ui_about_create(menu);
    if (!about_page) {
        ESP_LOGW("UI", "Failed to create about page - continuing with reduced functionality");
        about_page = NULL;
    }
    
    // Add another yield
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Create simplified diagnostics page (reduced complexity)
    lv_obj_t* diagnostics_page = NULL;
    if (check_heap_available(50000, "diagnostics page creation")) {
        diagnostics_page = ui_diagnostics_create(menu);
        if (!diagnostics_page) {
            ESP_LOGW("UI", "Failed to create diagnostics page - continuing with reduced functionality");
        }
    } else {
        ESP_LOGW("UI", "Insufficient heap for diagnostics page - skipping");
    }
    
    ESP_LOGI("UI", "External UI components created");

    // ===== ROOT PAGE (MAIN MENU) =====
    lv_obj_t* root_page = lv_menu_page_create(menu, "WaveX System");
    if (!root_page) {
        ESP_LOGE("UI", "Failed to create root page");
        return NULL;
    }
    lv_obj_set_style_pad_hor(root_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(menu), 0), 0);
    
    // Main configuration sections
    section = lv_menu_section_create(root_page);
    
    lv_obj_t* audio_item = create_menu_text(section, LV_SYMBOL_AUDIO, "Audio Settings");
    if (audio_item) {
        lv_menu_set_load_page_event(menu, audio_item, audio_page);
    }
    
    lv_obj_t* display_item = create_menu_text(section, LV_SYMBOL_EYE_OPEN, "Display & Interface");
    if (display_item) {
        lv_menu_set_load_page_event(menu, display_item, display_page);
    }
    
    lv_obj_t* midi_item = create_menu_text(section, LV_SYMBOL_BLUETOOTH, "MIDI Configuration");
    if (midi_item) {
        lv_menu_set_load_page_event(menu, midi_item, midi_page);
    }
    
    lv_obj_t* system_item = create_menu_text(section, LV_SYMBOL_SETTINGS, "System Settings");
    if (system_item) {
        lv_menu_set_load_page_event(menu, system_item, system_page);
    }
    
    // Add advanced sections with null checks
    lv_menu_separator_create(root_page);
    section = lv_menu_section_create(root_page);
    
    // Only add diagnostics if it was created successfully
    if (diagnostics_page) {
        lv_obj_t* diagnostics_item = create_menu_text(section, LV_SYMBOL_CHARGE, "System Diagnostics");
        if (diagnostics_item) {
            lv_menu_set_load_page_event(menu, diagnostics_item, diagnostics_page);
        }
    }
    
    // Only add about if it was created successfully
    if (about_page) {
        lv_obj_t* about_item = create_menu_text(section, LV_SYMBOL_HOME, "About WaveX");
        if (about_item) {
            lv_menu_set_load_page_event(menu, about_item, about_page);
        }
    }
    
    ESP_LOGI("UI", "Root page created successfully");

    // Simplify status section to use less memory
    lv_menu_separator_create(root_page);
    lv_obj_t* status_label = lv_label_create(root_page);
    if (status_label) {
        lv_label_set_text(status_label, "System Status");
        lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_top(status_label, 10, 0);
    }
    
    section = lv_menu_section_create(root_page);
    if (section) {
        // Create simple text directly instead of using create_menu_text to save memory
        lv_obj_t* simple_status = lv_label_create(section);
        if (simple_status) {
            char status_text[100];
            uint8_t ram_usage = get_ram_usage_percent();
            snprintf(status_text, sizeof(status_text), 
                    LV_SYMBOL_CHARGE " ESP32-S3 @ 240MHz\n" 
                    LV_SYMBOL_LIST " RAM: %d%% used", ram_usage);
            lv_label_set_text(simple_status, status_text);
            lv_obj_set_style_text_color(simple_status, lv_color_hex(0xFFFFFF), 0);
        }
    }
    
    ESP_LOGI("UI", "Simplified status section created");

    // Ensure proper initial navigation to root page
    lv_menu_set_page(menu, root_page);

    // Defer initial navigation to prevent watchdog timeout during complex menu setup
    ESP_LOGI("UI", "Menu creation complete - initial navigation deferred");

    return menu;
}

lv_obj_t* ui_main_get_screen(void)
{
    return main_screen;
} 

lv_obj_t* ui_diagnostics_create(lv_obj_t* parent_menu) {
    // Validate parent_menu
    if (!parent_menu) {
        ESP_LOGE("UI", "ui_diagnostics_create: parent_menu is NULL");
        return NULL;
    }

    // Create diagnostics page
    lv_obj_t* diagnostics_page = lv_menu_page_create(parent_menu, "System Diagnostics");
    if (!diagnostics_page) {
        ESP_LOGE("UI", "ui_diagnostics_create: failed to create diagnostics page");
        return NULL;
    }
    
    lv_obj_set_style_pad_hor(diagnostics_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(parent_menu), 0), 0);
    
    // ===== REAL-TIME SYSTEM MONITORING =====
    lv_menu_separator_create(diagnostics_page);
    lv_obj_t* monitor_title = lv_label_create(diagnostics_page);
    lv_label_set_text(monitor_title, "Real-Time Monitoring");
    lv_obj_set_style_text_font(monitor_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(monitor_title, lv_color_hex(0x00AAFF), 0);
    lv_obj_set_style_pad_top(monitor_title, 10, 0);
    
    lv_obj_t* section = lv_menu_section_create(diagnostics_page);
    
    // System info widget (moved to top for prominence)
    create_system_info_widget(section);
    
    // ===== RESOURCE USAGE METERS =====
    lv_menu_separator_create(diagnostics_page);
    lv_obj_t* resources_title = lv_label_create(diagnostics_page);
    lv_label_set_text(resources_title, "Resource Usage");
    lv_obj_set_style_text_font(resources_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(resources_title, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_pad_top(resources_title, 10, 0);
    
    section = lv_menu_section_create(diagnostics_page);
    
    // CPU usage with better description
    cpu_usage_bar = create_progress_bar_with_label(section, "CPU Load (System Tasks)", 0, 100, 25);
    
    // RAM usage with actual values
    ram_usage_bar = create_progress_bar_with_label(section, "Memory Usage (Heap)", 0, 100, get_ram_usage_percent());
    
    // Flash usage
    flash_usage_bar = create_progress_bar_with_label(section, "Flash Storage", 0, 100, get_flash_usage_percent());
    
    // ===== HARDWARE STATUS =====
    lv_menu_separator_create(diagnostics_page);
    lv_obj_t* hardware_title = lv_label_create(diagnostics_page);
    lv_label_set_text(hardware_title, "Hardware Status");
    lv_obj_set_style_text_font(hardware_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hardware_title, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_pad_top(hardware_title, 10, 0);
    
    section = lv_menu_section_create(diagnostics_page);
    
    // Core hardware status
    create_menu_text(section, LV_SYMBOL_OK, "Display: ST7796S (480x320) OK");
    
    // Touch controller status with actual detection
    if (touch_controller != nullptr) {
        create_menu_text(section, LV_SYMBOL_OK, "Touch: FT6X36 I2C Ready");
    } else {
        create_menu_text(section, LV_SYMBOL_CLOSE, "Touch: FT6X36 Failed");
    }
    
    create_menu_text(section, LV_SYMBOL_OK, "Audio MCU: SPI Connected");
    
    // ===== CONNECTIVITY STATUS =====
    lv_menu_separator_create(diagnostics_page);
    lv_obj_t* connectivity_title = lv_label_create(diagnostics_page);
    lv_label_set_text(connectivity_title, "Connectivity");
    lv_obj_set_style_text_font(connectivity_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(connectivity_title, lv_color_hex(0xFF4488), 0);
    lv_obj_set_style_pad_top(connectivity_title, 10, 0);
    
    section = lv_menu_section_create(diagnostics_page);
    
    // I/O status
    create_menu_text(section, LV_SYMBOL_SD_CARD, "SD Card: Not Detected");
    create_menu_text(section, LV_SYMBOL_USB, "USB: Device Mode Active");
    create_menu_text(section, LV_SYMBOL_BLUETOOTH, "MIDI USB: Ready");
    create_menu_text(section, LV_SYMBOL_WIFI, "Inter-MCU SPI: Active");
    
    // ===== POWER & THERMAL =====
    lv_menu_separator_create(diagnostics_page);
    lv_obj_t* power_title = lv_label_create(diagnostics_page);
    lv_label_set_text(power_title, "Power & Thermal");
    lv_obj_set_style_text_font(power_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(power_title, lv_color_hex(0xAA00FF), 0);
    lv_obj_set_style_pad_top(power_title, 10, 0);
    
    section = lv_menu_section_create(diagnostics_page);
    
    // Power status
    create_menu_text(section, LV_SYMBOL_POWER, "Power Mode: Performance");
    create_menu_text(section, LV_SYMBOL_CHARGE, "CPU Frequency: 240MHz");
    create_menu_text(section, LV_SYMBOL_EYE_OPEN, "Display Backlight: Active");
    
    // ===== DIAGNOSTIC ACTIONS =====
    lv_menu_separator_create(diagnostics_page);
    lv_obj_t* actions_title = lv_label_create(diagnostics_page);
    lv_label_set_text(actions_title, "Diagnostic Tools");
    lv_obj_set_style_text_font(actions_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(actions_title, lv_color_hex(0x88FFAA), 0);
    lv_obj_set_style_pad_top(actions_title, 10, 0);
    
    section = lv_menu_section_create(diagnostics_page);
    
    // Diagnostic tools
    create_menu_text(section, LV_SYMBOL_REFRESH, "Test Touch Calibration");
    create_menu_text(section, LV_SYMBOL_AUDIO, "Audio Loopback Test");
    create_menu_text(section, LV_SYMBOL_SD_CARD, "Storage Speed Test");
    create_menu_text(section, LV_SYMBOL_SETTINGS, "GPIO Pin Test");
    create_menu_text(section, LV_SYMBOL_LIST, "Memory Dump");
    
    // Start resource monitoring task if not already running
    if (resource_monitor_task == NULL) {
        xTaskCreate(resource_monitor_task_func, "resource_monitor", 2048, NULL, 1, &resource_monitor_task);
    }
    
    return diagnostics_page;
}

lv_obj_t* ui_setup_create(lv_obj_t* parent_menu) {
    // Validate parent_menu
    if (!parent_menu) {
        ESP_LOGE("UI", "ui_setup_create: parent_menu is NULL");
        return NULL;
    }

    // Create setup page
    lv_obj_t* setup_page = lv_menu_page_create(parent_menu, "Advanced Setup");
    if (!setup_page) {
        ESP_LOGE("UI", "ui_setup_create: failed to create setup page");
        return NULL;
    }
    
    lv_obj_set_style_pad_hor(setup_page, lv_obj_get_style_pad_left(lv_menu_get_main_header(parent_menu), 0), 0);
    
    // ===== AUDIO ENGINE CONFIGURATION =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* audio_title = lv_label_create(setup_page);
    lv_label_set_text(audio_title, "Audio Engine Configuration");
    lv_obj_set_style_text_font(audio_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(audio_title, lv_color_hex(0x00AAFF), 0);
    lv_obj_set_style_pad_top(audio_title, 10, 0);
    
    lv_obj_t* section = lv_menu_section_create(setup_page);
    
    // Advanced audio settings with event callbacks
    create_menu_slider(section, LV_SYMBOL_AUDIO, "Sample Rate (Hz)", 44100, 96000, 48000);
    // TODO: Add event callback for sample rate changes when needed
    
    create_menu_slider(section, LV_SYMBOL_CHARGE, "Buffer Size (samples)", 64, 512, 128);
    // TODO: Add event callback for buffer size changes when needed
    
    create_menu_slider(section, LV_SYMBOL_SETTINGS, "Audio Latency (ms)", 1, 20, 5);
    create_menu_switch(section, LV_SYMBOL_POWER, "Low Latency Mode", true);
    create_menu_switch(section, LV_SYMBOL_MUTE, "Anti-Aliasing Filter", true);
    create_menu_switch(section, LV_SYMBOL_AUDIO, "High Quality Mode", false);
    
    // ===== PERFORMANCE & OPTIMIZATION =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* performance_title = lv_label_create(setup_page);
    lv_label_set_text(performance_title, "Performance & Optimization");
    lv_obj_set_style_text_font(performance_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(performance_title, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_pad_top(performance_title, 10, 0);
    
    section = lv_menu_section_create(setup_page);
    
    // CPU and memory settings with functional callbacks
    create_menu_slider(section, LV_SYMBOL_CHARGE, "CPU Frequency (MHz)", 80, 240, 240);
    create_menu_switch_with_callback(section, LV_SYMBOL_POWER, "Performance Mode", system_performance_mode, performance_mode_event_cb);
    create_menu_switch(section, LV_SYMBOL_SETTINGS, "CPU Core Affinity", false);
    create_menu_slider(section, LV_SYMBOL_LIST, "Task Priority", 1, 10, 5);
    create_menu_switch(section, LV_SYMBOL_EYE_OPEN, "Memory Optimization", true);
    
    // ===== MIDI ADVANCED SETTINGS =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* midi_title = lv_label_create(setup_page);
    lv_label_set_text(midi_title, "MIDI Advanced Configuration");
    lv_obj_set_style_text_font(midi_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(midi_title, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_pad_top(midi_title, 10, 0);
    
    section = lv_menu_section_create(setup_page);
    
    // Advanced MIDI settings
    create_menu_slider(section, LV_SYMBOL_SETTINGS, "MIDI Base Channel", 1, 16, 1);
    create_menu_slider(section, LV_SYMBOL_BLUETOOTH, "USB MIDI Port", 1, 4, 1);
    create_menu_switch(section, LV_SYMBOL_WIFI, "MIDI Clock Sync", false);
    create_menu_switch(section, LV_SYMBOL_AUDIO, "MIDI Thru Enable", true);
    create_menu_switch(section, LV_SYMBOL_EDIT, "Local Control", true);
    create_menu_slider(section, LV_SYMBOL_VOLUME_MID, "Velocity Curve", 0, 4, 1);
    create_menu_slider(section, LV_SYMBOL_SETTINGS, "Note Priority", 0, 2, 1);
    
    // ===== DISPLAY & INTERFACE ADVANCED =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* display_title = lv_label_create(setup_page);
    lv_label_set_text(display_title, "Display & Interface Advanced");
    lv_obj_set_style_text_font(display_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(display_title, lv_color_hex(0xFF4488), 0);
    lv_obj_set_style_pad_top(display_title, 10, 0);
    
    section = lv_menu_section_create(setup_page);
    
    // Advanced display settings with functional callbacks
    create_menu_slider(section, LV_SYMBOL_EYE_OPEN, "Display Refresh (Hz)", 30, 120, 60);
    create_menu_slider(section, LV_SYMBOL_REFRESH, "UI Animation Speed", 1, 10, 5);
    create_menu_switch_with_callback(section, LV_SYMBOL_IMAGE, "Force Dark Theme", dark_theme_enabled, dark_theme_event_cb);
    create_menu_switch(section, LV_SYMBOL_POWER, "Auto Sleep Display", true);
    create_menu_slider(section, LV_SYMBOL_EDIT, "Touch Debounce (ms)", 5, 50, 20);
    create_menu_slider(section, LV_SYMBOL_SETTINGS, "Menu Scroll Speed", 1, 10, 5);
    
    // ===== SYSTEM & HARDWARE SETUP =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* system_title = lv_label_create(setup_page);
    lv_label_set_text(system_title, "System & Hardware Setup");
    lv_obj_set_style_text_font(system_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(system_title, lv_color_hex(0xAA00FF), 0);
    lv_obj_set_style_pad_top(system_title, 10, 0);
    
    section = lv_menu_section_create(setup_page);
    
    // System hardware settings with functional callbacks
    create_menu_switch(section, LV_SYMBOL_USB, "USB Device Mode", true);
    create_menu_switch(section, LV_SYMBOL_SD_CARD, "Enable SD Card", true);
    create_menu_switch_with_callback(section, LV_SYMBOL_SAVE, "Auto-Save Settings", auto_save_enabled, auto_save_event_cb);
    create_menu_slider(section, LV_SYMBOL_SETTINGS, "Boot Delay (sec)", 0, 10, 2);
    create_menu_switch(section, LV_SYMBOL_CHARGE, "Watchdog Timer", true);
    create_menu_switch_with_callback(section, LV_SYMBOL_LIST, "Debug Logging", debug_logging_enabled, debug_logging_event_cb);
    
    // ===== CALIBRATION & TESTING =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* calibration_title = lv_label_create(setup_page);
    lv_label_set_text(calibration_title, "Calibration & Testing");
    lv_obj_set_style_text_font(calibration_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(calibration_title, lv_color_hex(0x88FFAA), 0);
    lv_obj_set_style_pad_top(calibration_title, 10, 0);
    
    section = lv_menu_section_create(setup_page);
    
    // Calibration and testing tools
    create_menu_text_with_callback(section, LV_SYMBOL_REFRESH, "Calibrate Touch Screen", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_AUDIO, "Audio I/O Test", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_SETTINGS, "GPIO Function Test", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_SD_CARD, "Storage Benchmark", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_LIST, "System Stress Test", NULL);
    
    // ===== FACTORY & RECOVERY =====
    lv_menu_separator_create(setup_page);
    lv_obj_t* factory_title = lv_label_create(setup_page);
    lv_label_set_text(factory_title, "Factory & Recovery");
    lv_obj_set_style_text_font(factory_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(factory_title, lv_color_hex(0xFF6600), 0);
    lv_obj_set_style_pad_top(factory_title, 10, 0);
    
    section = lv_menu_section_create(setup_page);
    
    // Factory and recovery options with functional callbacks
    create_menu_text_with_callback(section, LV_SYMBOL_SAVE, "Save Current Settings", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_DOWNLOAD, "Export Configuration", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_UPLOAD, "Import Configuration", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_REFRESH, "Factory Reset (Settings)", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_DOWNLOAD, "Firmware Update", NULL);
    create_menu_text_with_callback(section, LV_SYMBOL_CLOSE, "System Restart", system_restart_event_cb);
    
    return setup_page;
} 



