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
#include <stdio.h>

static lv_obj_t* main_screen = NULL;
static lv_obj_t* main_menu = NULL;

// LCD and touch handles
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;
static FT6X36* touch_controller = NULL;
static lv_display_t* lvgl_disp = NULL;
static lv_indev_t* lvgl_touch_indev = NULL;

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

// Touch state variables for LVGL integration
static TPoint current_touch_point = {0, 0};
static TEvent current_touch_event = TEvent::None;
static bool touch_pressed = false;

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
 * @brief Touch read callback for LVGL input device
 * Bridges FT6X36 touch controller with LVGL input system
 */
static void wavex_touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    // Poll the touch controller for new data
    if (touch_controller != nullptr) {
        TPoint point;
        TEvent event;
        touch_controller->poll(&point, &event);
        
        // Update our touch state
        if (event != TEvent::None) {
            wavex_touch_event_handler(point, event);
        }
    }
    
    // Set LVGL touch data
    if (touch_pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = current_touch_point.x;
        data->point.y = current_touch_point.y;
        
        // Apply coordinate transformations to match display orientation
        // The display is configured with swap_xy=true, mirror_x=true
        // so we need to apply the same transformations to touch coordinates
        
        // Swap X and Y coordinates
        int16_t temp = data->point.x;
        data->point.x = data->point.y;
        data->point.y = temp;
        
        // Mirror X coordinate
        data->point.x = WAVEX_LCD_H_RES - data->point.x;
        
        // Ensure coordinates are within bounds
        if (data->point.x < 0) data->point.x = 0;
        if (data->point.x >= WAVEX_LCD_H_RES) data->point.x = WAVEX_LCD_H_RES - 1;
        if (data->point.y < 0) data->point.y = 0;
        if (data->point.y >= WAVEX_LCD_V_RES) data->point.y = WAVEX_LCD_V_RES - 1;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    
    // Continue reading
    data->continue_reading = false;
}

/**
 * @brief Initialize ST7796S display hardware
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t wavex_display_init(void)
{
    ESP_LOGI("DISPLAY", "Initializing ST7796S display...");
    
    // Configure backlight GPIO
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << WAVEX_LCD_GPIO_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), "DISPLAY", "Backlight GPIO config failed");
    
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
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
            .octal_mode = 0,
            .quad_mode = 0,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0
        }
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)WAVEX_SPI_HOST, &io_config, &lcd_io),
                       "DISPLAY", "Panel IO creation failed");
    
    // Install ST7796S LCD driver
    ESP_LOGI("DISPLAY", "Installing LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = WAVEX_LCD_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = WAVEX_LCD_BITS_PER_PIXEL,
        .flags = {0},
        .vendor_config = NULL
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(lcd_io, &panel_config, &lcd_panel),
                       "DISPLAY", "LCD panel creation failed");
    
    // Reset and initialize panel
    ESP_LOGI("DISPLAY", "Resetting and initializing panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), "DISPLAY", "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), "DISPLAY", "Panel init failed");
    
    // Configure display orientation (landscape mode for 480x320)
    // REMOVED: Conflicting rotation settings that cause display corruption
    // ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, true, false), "DISPLAY", "Panel mirror failed");
    // ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(lcd_panel, true), "DISPLAY", "Panel swap_xy failed");
    
    // Turn on display
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), "DISPLAY", "Panel display on failed");
    
    // Turn on backlight
    ESP_LOGI("DISPLAY", "Enabling backlight on GPIO21...");
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_LCD_GPIO_BL, WAVEX_LCD_BL_ON_LEVEL), 
                       "DISPLAY", "Backlight enable failed");
    ESP_LOGI("DISPLAY", "Backlight enabled successfully");
    
    // Verify backlight state
    int bl_state = gpio_get_level(WAVEX_LCD_GPIO_BL);
    ESP_LOGI("DISPLAY", "Backlight pin state: %d", bl_state);
    
    // REMOVED: Test pattern that was covering UI content
    // ESP_LOGI("DISPLAY", "Testing display with color pattern...");
    // uint16_t* test_buffer = (uint16_t*)heap_caps_malloc(WAVEX_LCD_H_RES * 10 * sizeof(uint16_t), MALLOC_CAP_DMA);
    // if (test_buffer) {
    //     // Create a simple test pattern (red, green, blue stripes)
    //     for (int i = 0; i < WAVEX_LCD_H_RES * 10; i++) {
    //         if (i < WAVEX_LCD_H_RES * 3) {
    //             test_buffer[i] = 0xF800;  // Red
    //         } else if (i < WAVEX_LCD_H_RES * 6) {
    //             test_buffer[i] = 0x07E0;  // Green
    //         } else {
    //             test_buffer[i] = 0x001F;  // Blue
    //         }
    //     }
    //     
    //     // Send test pattern to display
    //     esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, WAVEX_LCD_H_RES, 10, test_buffer);
    //     vTaskDelay(pdMS_TO_TICKS(100));  // Wait for pattern to be visible
    //     
    //     free(test_buffer);
    //     ESP_LOGI("DISPLAY", "Test pattern sent to display");
    // }
    
    ESP_LOGI("DISPLAY", "ST7796S display initialization complete");
    return ESP_OK;
}

/**
 * @brief Initialize FT6X36 I2C capacitive touch controller hardware
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t wavex_touch_init(void)
{
    ESP_LOGI("TOUCH", "Initializing FT6X36 I2C capacitive touch controller...");
    
    // Configure I2C bus for touch controller
    ESP_LOGI("TOUCH", "Configuring I2C bus");
    i2c_config_t i2c_conf = {};
    i2c_conf.mode = I2C_MODE_MASTER;
    i2c_conf.sda_io_num = WAVEX_CTP_GPIO_SDA;      // GPIO20 (J3 pin 19)
    i2c_conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.scl_io_num = WAVEX_CTP_GPIO_SCL;      // GPIO1 (J3 pin 4)
    i2c_conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.master.clk_speed = WAVEX_CTP_I2C_FREQ_HZ;  // 100kHz
    i2c_conf.clk_flags = 0;
    
    ESP_RETURN_ON_ERROR(i2c_param_config(WAVEX_CTP_I2C_NUM, &i2c_conf), 
                       "TOUCH", "I2C parameter config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(WAVEX_CTP_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0),
                       "TOUCH", "I2C driver installation failed");
    
    // Configure touch reset GPIO (optional but recommended)
    gpio_config_t touch_rst_config = {
        .pin_bit_mask = 1ULL << WAVEX_CTP_GPIO_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&touch_rst_config), "TOUCH", "Touch reset GPIO config failed");
    
    // Reset touch controller
    ESP_LOGI("TOUCH", "Resetting touch controller");
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_CTP_GPIO_RST, 0), "TOUCH", "Touch reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));  // Hold reset for 10ms
    ESP_RETURN_ON_ERROR(gpio_set_level(WAVEX_CTP_GPIO_RST, 1), "TOUCH", "Touch reset high failed");
    vTaskDelay(pdMS_TO_TICKS(50));  // Wait for controller to initialize
    
    // Initialize FT6X36 touch controller with interrupt pin
    ESP_LOGI("TOUCH", "Installing FT6X36 touch driver");
    touch_controller = new FT6X36(GPIO_NUM_NC);  // No interrupt pin for polling mode
    if (touch_controller == nullptr) {
        ESP_LOGE("TOUCH", "Failed to create FT6X36 touch controller");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize the touch controller (uses default threshold and screen size)
    if (!touch_controller->begin(FT6X36_DEFAULT_THRESHOLD, WAVEX_LCD_H_RES, WAVEX_LCD_V_RES)) {
        ESP_LOGE("TOUCH", "Failed to initialize FT6X36 touch controller");
        delete touch_controller;
        touch_controller = nullptr;
        return ESP_FAIL;
    }
    
    // Set touch controller rotation to match display
    touch_controller->setRotation(1);  // Match display rotation
    touch_controller->setTouchWidth(WAVEX_LCD_H_RES);
    touch_controller->setTouchHeight(WAVEX_LCD_V_RES);
    
    ESP_LOGI("TOUCH", "FT6X36 I2C capacitive touch controller initialization complete");
    return ESP_OK;
}

void ui_main_init(void)
{
    ESP_LOGI("UI", "Initializing esp_lvgl_port...");
    
    // Initialize esp_lvgl_port with display and touch
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 8192,  // Increased from 6144 to fix stack overflow
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    
    // Initialize ST7796S display hardware
    ESP_LOGI("UI", "Initializing display hardware...");
    ESP_ERROR_CHECK(wavex_display_init());
    
    // Add display to LVGL port
    ESP_LOGI("UI", "Configuring LVGL display...");
    uint32_t buffer_size = WAVEX_LCD_H_RES * WAVEX_LVGL_DRAW_BUF_HEIGHT;
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .control_handle = NULL,
        .buffer_size = buffer_size,
        .double_buffer = WAVEX_LVGL_DOUBLE_BUFFER,
        .trans_size = 0,
        .hres = WAVEX_LCD_H_RES,
        .vres = WAVEX_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,   // Flip horizontally for 180° rotation
            .mirror_y = true,   // Flip vertically for 180° rotation
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
            .full_refresh = false,
            .direct_mode = false
        }
    };
    
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (lvgl_disp == NULL) {
        ESP_LOGE("UI", "Failed to add display to LVGL port");
        return;
    }
    
    ESP_LOGI("UI", "Display initialization complete - %dx%d @ %d bpp", 
             WAVEX_LCD_H_RES, WAVEX_LCD_V_RES, WAVEX_LCD_BITS_PER_PIXEL);
    
    // Initialize FT6X36 I2C touch controller hardware
    ESP_LOGI("UI", "Initializing touch hardware...");
    ESP_ERROR_CHECK(wavex_touch_init());
    
    // Create custom touch input device for LVGL (since FT6X36 is not directly compatible with esp_lvgl_port)
    ESP_LOGI("UI", "Configuring LVGL touch input...");
    
    // Create LVGL input device for touch
    lvgl_touch_indev = lv_indev_create();
    lv_indev_set_type(lvgl_touch_indev, LV_INDEV_TYPE_POINTER);
    
    // Set touch read callback to bridge FT6X36 with LVGL
    lv_indev_set_read_cb(lvgl_touch_indev, wavex_touch_read_cb);
    
    if (lvgl_touch_indev == NULL) {
        ESP_LOGE("UI", "Failed to create LVGL touch input device");
        return;
    }
    
    ESP_LOGI("UI", "Touch initialization complete - FT6X36 I2C @ %dHz", 
             WAVEX_CTP_I2C_FREQ_HZ);
    
    ESP_LOGI("UI", "Creating main UI elements...");
    
    // Lock LVGL for thread safety
    lvgl_port_lock(0);
    
    // Create main screen
    main_screen = lv_obj_create(NULL);
    if (main_screen == NULL) {
        ESP_LOGE("UI", "Failed to create main screen");
        lvgl_port_unlock();
        return;
    }
    ESP_LOGI("UI", "Main screen created successfully");
    
    // Set background color to a visible color (not black)
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x003a57), LV_PART_MAIN);
    lv_obj_set_style_text_color(main_screen, lv_color_hex(0xffffff), LV_PART_MAIN);
    ESP_LOGI("UI", "Main screen background color set");
    
    // Create the main menu
    ESP_LOGI("UI", "Creating main menu...");
    main_menu = ui_main_create_menu();
    if (main_menu == NULL) {
        ESP_LOGE("UI", "Failed to create main menu");
        lvgl_port_unlock();
        return;
    }
    ESP_LOGI("UI", "Main menu created successfully");
    
    // Load the main screen
    ESP_LOGI("UI", "Loading main screen...");
    lv_screen_load(main_screen);
    ESP_LOGI("UI", "Main screen loaded successfully");
    
    // Force a refresh to ensure content is displayed
    ESP_LOGI("UI", "Forcing display refresh...");
    lv_obj_invalidate(main_screen);
    lv_refr_now(lvgl_disp);
    ESP_LOGI("UI", "Display refresh completed");
    
    // Add a simple test label to verify rendering
    ESP_LOGI("UI", "Adding test label...");
    lv_obj_t* test_label = lv_label_create(main_screen);
    if (test_label) {
        lv_label_set_text(test_label, "WaveX Test");
        lv_obj_set_pos(test_label, 10, 10);
        lv_obj_set_style_text_color(test_label, lv_color_hex(0xFFFFFF), 0);
        ESP_LOGI("UI", "Test label created and positioned");
    } else {
        ESP_LOGE("UI", "Failed to create test label");
    }
    
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