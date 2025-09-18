/**
 * @file ui_task.c
 * @brief UI Task Implementation for MIPI DSI Display with LVGL
 * 
 * This implementation provides full LVGL integration with MIPI DSI display
 * using the Waveshare 5-DSI-TOUCH-A display and HX8394 driver.
 */

 #include "ui_task.h"
 #include "esp_log.h"
 #include "esp_check.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/semphr.h"
 #include "driver/gpio.h"
 #include "driver/i2c_master.h"
 #include "hardware_pins.h"
 
// LVGL includes
#include "esp_lvgl_port.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_io_i2c.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

// LVGL port lock macros for thread safety
#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

// Include BSP header for display functions
#include "bsp/esp32_p4_nano.h"
 
#define LV_TICK_PERIOD_MS 20
 
 static const char *TAG = "UI_TASK";
 
 // Task handle
 static TaskHandle_t s_ui_task_handle = NULL;
 
// Display and LVGL handles
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_lvgl_display = NULL;
static esp_timer_handle_t s_lvgl_tick_timer_handle = NULL;
 
// I2C handles
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
 
// Forward declarations
static void ui_task(void *pvParameters);
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static void lvgl_tick_cb(void *arg);
static esp_err_t init_touch_controller(void);
static esp_err_t init_lvgl_display(void);
static esp_err_t test_backlight(void);
static esp_err_t test_display_pattern(void);
static esp_err_t test_full_screen_pattern(void);
static esp_err_t test_color_patterns(void);
static void monitor_display_pins(void);

// Menu system functions
static void create_main_menu(lv_obj_t *parent);
static void create_system_menu(lv_obj_t *parent);
static void create_diagnostics_page(lv_obj_t *parent);
static void menu_button_event_cb(lv_event_t *e);
static void touch_event_cb(lv_event_t *e);
 
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
        .pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15),  // RST and INT pins
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure touch GPIO pins");

    // --- GT911 Reset and Address Selection Sequence ---
    // Per datasheet, this selects the I2C address. We will select 0x5D.
    gpio_set_level(GPIO_NUM_15, 1);  // INT high for address 0x5D
    gpio_set_level(GPIO_NUM_14, 0);  // RST low
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_14, 1);  // RST high
    vTaskDelay(pdMS_TO_TICKS(60)); // Hold INT state for >50ms after reset

    // Release INT pin to be an input for interrupts
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_15);  // Only INT pin
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
        .rst_gpio_num = GPIO_NUM_14,  // Touch reset pin
        .int_gpio_num = GPIO_NUM_15,  // Touch interrupt pin
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

    // Use BSP's LVGL display setup with minimal config to prevent underruns
    ESP_LOGI(TAG, "Using BSP's LVGL display setup...");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 720 * 5,    // Very conservative: 5 lines × 720px × 2B = 7.2KB
        .double_buffer = false,   // Disable double buffering to reduce memory bandwidth
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,   // Enable software rotation for HX8394
        }
    };

    s_lvgl_display = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(s_lvgl_display, ESP_FAIL, TAG, "Failed to start BSP display");

    // BSP disabled mirror operations for HX8394, so we need software rotation
    // Try 0 rotation first to isolate issues
    LV_LOCK();
    lv_display_set_rotation(s_lvgl_display, LV_DISPLAY_ROTATION_0);
    LV_UNLOCK();

    // BSP handles touch setup automatically
    ESP_LOGI(TAG, "LVGL display initialized successfully");
    return ESP_OK;
 }
 
// Touch callback is handled automatically by lvgl_port

/**
 * @brief Test backlight functionality
 */
static esp_err_t test_backlight(void)
{
    ESP_LOGI(TAG, "Testing backlight functionality...");
    
    // Test backlight pin configuration
    int backlight_level = gpio_get_level(WAVEX_ESP_DSI_BL);
    ESP_LOGI(TAG, "Current backlight level: %d", backlight_level);
    
    // Blink backlight to test control
    ESP_LOGI(TAG, "Blinking backlight 3 times...");
    for (int i = 0; i < 3; i++) {
        gpio_set_level(WAVEX_ESP_DSI_BL, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(WAVEX_ESP_DSI_BL, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Backlight blink %d/3", i + 1);
    }
    
    // Set backlight on
    gpio_set_level(WAVEX_ESP_DSI_BL, 1);
    ESP_LOGI(TAG, "Backlight test completed - should be ON now");
    return ESP_OK;
}

/**
 * @brief Test display with simple pattern using BSP
 */
static esp_err_t test_display_pattern(void)
{
    ESP_LOGI(TAG, "Testing display with simple pattern...");

    // Get panel handle from BSP if not already available
    if (s_panel_handle == NULL) {
        ESP_LOGI(TAG, "Getting panel handle from BSP...");
        // Use BSP's display_new function to get handles
        bsp_display_config_t config = {0}; // Default config
        ESP_RETURN_ON_ERROR(bsp_display_new(&config, &s_panel_handle, NULL),
                           TAG, "Failed to get panel handle from BSP");
    }
    
    // Create a simple test pattern - fill screen with different colors
    uint16_t *test_buffer = malloc(800 * 40 * sizeof(uint16_t));
    if (test_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate test buffer");
        return ESP_ERR_NO_MEM;
    }
    
    // Test pattern: red, green, blue stripes
    for (int y = 0; y < 40; y++) {
        for (int x = 0; x < 800; x++) {
            if (x < 266) {
                test_buffer[y * 800 + x] = 0xF800; // Red
            } else if (x < 533) {
                test_buffer[y * 800 + x] = 0x07E0; // Green
            } else {
                test_buffer[y * 800 + x] = 0x001F; // Blue
            }
        }
    }
    
    // Send test pattern to display
    ESP_LOGI(TAG, "Sending test pattern to display...");
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, 800, 40, test_buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw test pattern: %s", esp_err_to_name(ret));
        free(test_buffer);
        // Don't fail completely, just log and continue
        ESP_LOGW(TAG, "Test pattern failed, but continuing...");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Test pattern sent successfully");
    free(test_buffer);
    
    // Wait for display to finish rendering
    vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_OK;
}

/**
 * @brief Test display with full screen pattern using BSP
 */
static esp_err_t test_full_screen_pattern(void)
{
    ESP_LOGI(TAG, "Testing full screen pattern...");

    // Get panel handle from BSP if not already available
    if (s_panel_handle == NULL) {
        ESP_LOGI(TAG, "Getting panel handle from BSP...");
        // Use BSP's display_new function to get handles
        bsp_display_config_t config = {0}; // Default config
        ESP_RETURN_ON_ERROR(bsp_display_new(&config, &s_panel_handle, NULL),
                           TAG, "Failed to get panel handle from BSP");
    }
    
    // Create a full screen test pattern
    uint16_t *test_buffer = malloc(800 * 480 * sizeof(uint16_t));
    if (test_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate full screen test buffer");
        return ESP_ERR_NO_MEM;
    }
    
    // Test pattern: white screen with black border
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 800; x++) {
            if (x < 10 || x > 790 || y < 10 || y > 470) {
                test_buffer[y * 800 + x] = 0x0000; // Black border
            } else {
                test_buffer[y * 800 + x] = 0xFFFF; // White center
            }
        }
    }
    
    // Send full screen pattern to display
    ESP_LOGI(TAG, "Sending full screen pattern to display...");
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, 800, 480, test_buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw full screen pattern: %s", esp_err_to_name(ret));
        free(test_buffer);
        // Don't fail completely, just log and continue
        ESP_LOGW(TAG, "Full screen pattern failed, but continuing...");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Full screen pattern sent successfully");
    free(test_buffer);
    
    // Wait for display to finish rendering full screen
    vTaskDelay(pdMS_TO_TICKS(2000));
    return ESP_OK;
}

/**
 * @brief Test different color patterns using BSP
 */
static esp_err_t test_color_patterns(void)
{
    ESP_LOGI(TAG, "Testing color patterns...");

    // Get panel handle from BSP if not already available
    if (s_panel_handle == NULL) {
        ESP_LOGI(TAG, "Getting panel handle from BSP...");
        // Use BSP's display_new function to get handles
        bsp_display_config_t config = {0}; // Default config
        ESP_RETURN_ON_ERROR(bsp_display_new(&config, &s_panel_handle, NULL),
                           TAG, "Failed to get panel handle from BSP");
    }
    
    // Test solid colors
    uint16_t colors[] = {
        0x0000, // Black
        0xFFFF, // White
        0xF800, // Red
        0x07E0, // Green
        0x001F, // Blue
        0xFFE0, // Yellow
        0xF81F, // Magenta
        0x07FF, // Cyan
    };
    
    const char* color_names[] = {
        "Black", "White", "Red", "Green", "Blue", "Yellow", "Magenta", "Cyan"
    };
    
    for (int i = 0; i < 8; i++) {
        ESP_LOGI(TAG, "Testing %s color...", color_names[i]);
        
        // Create a small test area
        uint16_t *test_buffer = malloc(200 * 100 * sizeof(uint16_t));
        if (test_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate test buffer");
            return ESP_ERR_NO_MEM;
        }
        
        // Fill with solid color
        for (int y = 0; y < 100; y++) {
            for (int x = 0; x < 200; x++) {
                test_buffer[y * 200 + x] = colors[i];
            }
        }
        
        // Send to display
        esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 300, 190, 500, 290, test_buffer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to draw %s color: %s", color_names[i], esp_err_to_name(ret));
            free(test_buffer);
            // Don't return error, just continue with next color
            vTaskDelay(pdMS_TO_TICKS(2000)); // Wait longer if there's an error
            continue;
        }
        
        ESP_LOGI(TAG, "%s color drawn successfully", color_names[i]);
        free(test_buffer);
        vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds between colors for display to finish
    }
    
    ESP_LOGI(TAG, "Color pattern test completed");
    return ESP_OK;
}

/**
 * @brief Monitor display pin states
 */
static void monitor_display_pins(void)
{
    ESP_LOGI(TAG, "Display pin status:");
    ESP_LOGI(TAG, "  Reset pin (GPIO%d): %d", WAVEX_ESP_DSI_RST, gpio_get_level(WAVEX_ESP_DSI_RST));
    ESP_LOGI(TAG, "  Backlight pin (GPIO%d): %d", WAVEX_ESP_DSI_BL, gpio_get_level(WAVEX_ESP_DSI_BL));
    ESP_LOGI(TAG, "  DSI D0P (GPIO%d): %d", WAVEX_ESP_DSI_D0P, gpio_get_level(WAVEX_ESP_DSI_D0P));
    ESP_LOGI(TAG, "  DSI D0N (GPIO%d): %d", WAVEX_ESP_DSI_D0N, gpio_get_level(WAVEX_ESP_DSI_D0N));
    ESP_LOGI(TAG, "  DSI D1P (GPIO%d): %d", WAVEX_ESP_DSI_D1P, gpio_get_level(WAVEX_ESP_DSI_D1P));
    ESP_LOGI(TAG, "  DSI D1N (GPIO%d): %d", WAVEX_ESP_DSI_D1N, gpio_get_level(WAVEX_ESP_DSI_D1N));
    ESP_LOGI(TAG, "  DSI CLKP (GPIO%d): %d", WAVEX_ESP_DSI_CLKP, gpio_get_level(WAVEX_ESP_DSI_CLKP));
    ESP_LOGI(TAG, "  DSI CLKN (GPIO%d): %d", WAVEX_ESP_DSI_CLKN, gpio_get_level(WAVEX_ESP_DSI_CLKN));
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

    LV_LOCK();
    // Clear parent content
    lv_obj_clean(parent);

    // Create a flex container for menu buttons
    lv_obj_t *menu_cont = lv_obj_create(parent);
    lv_obj_set_size(menu_cont, lv_pct(90), lv_pct(80));
    lv_obj_set_style_bg_color(menu_cont, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_cont, lv_color_make(0xE0, 0xE0, 0xE0), LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu_cont, 20, LV_PART_MAIN);
    lv_obj_center(menu_cont);

    // Set flex layout
    lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create menu buttons
    lv_obj_t *btn1 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn1, lv_pct(100), 60);
    lv_obj_add_event_cb(btn1, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"system");
    lv_obj_t *label1 = lv_label_create(btn1);
    lv_label_set_text(label1, "System");
    lv_obj_center(label1);

    lv_obj_t *btn2 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn2, lv_pct(100), 60);
    lv_obj_add_event_cb(btn2, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"audio");
    lv_obj_t *label2 = lv_label_create(btn2);
    lv_label_set_text(label2, "Audio");
    lv_obj_center(label2);

    lv_obj_t *btn3 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn3, lv_pct(100), 60);
    lv_obj_add_event_cb(btn3, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"network");
    lv_obj_t *label3 = lv_label_create(btn3);
    lv_label_set_text(label3, "Network");
    lv_obj_center(label3);

    lv_obj_t *btn4 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn4, lv_pct(100), 60);
    lv_obj_add_event_cb(btn4, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"settings");
    lv_obj_t *label4 = lv_label_create(btn4);
    lv_label_set_text(label4, "Settings");
    lv_obj_center(label4);
    
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

    LV_LOCK();
    // Clear parent content
    lv_obj_clean(parent);

    // Create a flex container for system options
    lv_obj_t *menu_cont = lv_obj_create(parent);
    lv_obj_set_size(menu_cont, lv_pct(90), lv_pct(80));
    lv_obj_set_style_bg_color(menu_cont, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(menu_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(menu_cont, lv_color_make(0xE0, 0xE0, 0xE0), LV_PART_MAIN);
    lv_obj_set_style_pad_all(menu_cont, 20, LV_PART_MAIN);
    lv_obj_center(menu_cont);

    // Set flex layout
    lv_obj_set_flex_flow(menu_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(menu_cont);
    lv_obj_set_size(back_btn, lv_pct(100), 50);
    lv_obj_add_event_cb(back_btn, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"back_main");
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back to Main Menu");
    lv_obj_center(back_label);

    // System options
    lv_obj_t *btn1 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn1, lv_pct(100), 60);
    lv_obj_add_event_cb(btn1, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"diagnostics");
    lv_obj_t *label1 = lv_label_create(btn1);
    lv_label_set_text(label1, "Diagnostics");
    lv_obj_center(label1);

    lv_obj_t *btn2 = lv_btn_create(menu_cont);
    lv_obj_set_size(btn2, lv_pct(100), 60);
    lv_obj_add_event_cb(btn2, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"info");
    lv_obj_t *label2 = lv_label_create(btn2);
    lv_label_set_text(label2, "System Info");
    lv_obj_center(label2);
    
    LV_UNLOCK();
}

/**
 * @brief Create the diagnostics page
 */
static void create_diagnostics_page(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "create_diagnostics_page: parent is NULL");
        return;
    }

    LV_LOCK();
    // Clear parent content
    lv_obj_clean(parent);

    // Create diagnostics container
    lv_obj_t *diag_cont = lv_obj_create(parent);
    lv_obj_set_size(diag_cont, lv_pct(95), lv_pct(85));
    lv_obj_set_style_bg_color(diag_cont, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(diag_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(diag_cont, lv_color_make(0xE0, 0xE0, 0xE0), LV_PART_MAIN);
    lv_obj_set_style_pad_all(diag_cont, 15, LV_PART_MAIN);
    lv_obj_center(diag_cont);

    // Title
    lv_obj_t *title = lv_label_create(diag_cont);
    lv_label_set_text(title, "System Diagnostics");
    lv_obj_set_style_text_font(title, lv_font_get_default(), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x2E, 0x34, 0x40), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(diag_cont);
    lv_obj_set_size(back_btn, 100, 40);
    lv_obj_add_event_cb(back_btn, menu_button_event_cb, LV_EVENT_CLICKED, (void*)"back_system");
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);

    // Create scrollable content area
    lv_obj_t *content = lv_obj_create(diag_cont);
    lv_obj_set_size(content, lv_pct(100), lv_pct(80));
    lv_obj_set_style_bg_color(content, lv_color_make(0xF8, 0xF9, 0xFA), LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 1, LV_PART_MAIN);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 10);
    lv_obj_set_style_pad_all(content, 10, LV_PART_MAIN);

    // Add diagnostic information
    lv_obj_t *info_label = lv_label_create(content);
    char diag_text[512];
    snprintf(diag_text, sizeof(diag_text),
        "Display: HX8394 720x1280\n"
        "Touch: GT911 Controller\n"
        "Memory: %zu KB free\n"
        "System: ESP32-P4\n"
        "Status: Operational\n\n"
        "Touch the screen to test\n"
        "touchscreen functionality!",
        (size_t)(esp_get_free_heap_size() / 1024));
    lv_label_set_text(info_label, diag_text);
    lv_obj_set_style_text_font(info_label, lv_font_get_default(), LV_PART_MAIN);
    lv_obj_align(info_label, LV_ALIGN_TOP_LEFT, 0, 0);

    // Add touch test area
    lv_obj_t *touch_area = lv_obj_create(content);
    lv_obj_set_size(touch_area, lv_pct(100), 80);
    lv_obj_set_style_bg_color(touch_area, lv_color_make(0xE3, 0xF2, 0xFD), LV_PART_MAIN);
    lv_obj_set_style_border_width(touch_area, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(touch_area, lv_color_make(0x21, 0x96, 0xF3), LV_PART_MAIN);
    lv_obj_align(touch_area, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *touch_label = lv_label_create(touch_area);
    lv_label_set_text(touch_label, "Touch Test Area\nTap here to test touchscreen!");
    lv_obj_set_style_text_color(touch_label, lv_color_make(0x15, 0x65, 0xC0), LV_PART_MAIN);
    lv_obj_center(touch_label);

    // Add touch event handler
    lv_obj_add_event_cb(touch_area, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch_area, touch_event_cb, LV_EVENT_RELEASED, NULL);
    
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

        lv_obj_t *parent = lv_obj_get_parent(lv_event_get_target(e));
        while (parent && parent != lv_screen_active()) {
            parent = lv_obj_get_parent(parent);
        }

        if (parent == NULL) {
            ESP_LOGE(TAG, "Failed to find screen parent for menu navigation");
            return;
        }

        if (strcmp(btn_id, "system") == 0) {
            create_system_menu(parent);
        } else if (strcmp(btn_id, "diagnostics") == 0) {
            create_diagnostics_page(parent);
        } else if (strcmp(btn_id, "back_main") == 0) {
            create_main_menu(parent);
        } else if (strcmp(btn_id, "back_system") == 0) {
            create_system_menu(parent);
        } else {
            ESP_LOGI(TAG, "Menu option '%s' selected", btn_id);
        }
    }
}

/**
 * @brief Touch event callback for testing
 */
static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "Touch pressed on test area");
        // Note: Event callbacks run in LVGL context, no locking needed
        lv_obj_set_style_bg_color(obj, lv_color_make(0xFF, 0xEB, 0x3B), LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label) {
            lv_label_set_text(label, "Touch Detected!\nKeep pressing...");
        }
    } else if (code == LV_EVENT_RELEASED) {
        ESP_LOGI(TAG, "Touch released from test area");
        // Note: Event callbacks run in LVGL context, no locking needed
        lv_obj_set_style_bg_color(obj, lv_color_make(0xE3, 0xF2, 0xFD), LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label) {
            lv_label_set_text(label, "Touch Test Area\nTap here to test touchscreen!");
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

    // Create the main UI
    ESP_LOGI(TAG, "Creating main UI...");
    LV_LOCK();
    lv_obj_t *main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(main_screen, lv_color_white(), LV_PART_MAIN);
    
    // Create header
    lv_obj_t *header = lv_obj_create(main_screen);
    lv_obj_set_size(header, lv_pct(100), 80);
    lv_obj_set_style_bg_color(header, lv_color_make(0x2E, 0x34, 0x40), LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    
    // Add title to header
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "WaveX System");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, lv_font_get_default(), LV_PART_MAIN);
    lv_obj_center(title);
    
    // Create content area
    lv_obj_t *content = lv_obj_create(main_screen);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100) - 80);
    lv_obj_set_style_bg_color(content, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    // Create main menu
    create_main_menu(content);
    LV_UNLOCK();

    ESP_LOGI(TAG, "Main UI created successfully");

    // Main UI loop
    ESP_LOGI(TAG, "UI loop started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Short delay for responsiveness
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