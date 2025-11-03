#include "ui/display_manager.h"

#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp/esp32_p4_nano.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config/pin_config.h"
#include "esp_heap_caps.h"

#undef esp_lcd_new_panel_io_i2c
#define esp_lcd_new_panel_io_i2c esp_lcd_new_panel_io_i2c_v2

namespace wavex_ui {
namespace {

constexpr uint32_t LV_TICK_PERIOD_MS = 5;
static const char* TAG = "DisplayManager";

#define LV_LOCK()   lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

static void lvgl_tick_cb(void* /*arg*/)
{
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

extern "C" void wavex_lvgl_log_cb(signed char level, const char * buf)
{
    switch (level) {
        case 0: // TRACE
        case 1: // INFO
            ESP_LOGI("LVGL", "%s", buf);
            break;
        case 2: // WARN
            ESP_LOGW("LVGL", "%s", buf);
            break;
        case 3: // ERROR
            ESP_LOGE("LVGL", "%s", buf);
            break;
        default:
            ESP_LOGI("LVGL", "%s", buf);
            break;
    }
}

} // namespace

DisplayManager& DisplayManager::instance()
{
    static DisplayManager inst;
    return inst;
}

esp_err_t DisplayManager::init()
{
    if (display_) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(initLvglDisplay(), TAG, "failed to initialise LVGL display");
    ESP_RETURN_ON_ERROR(initTouchController(), TAG, "failed to initialise touch controller");
    ESP_RETURN_ON_ERROR(startLvglTick(), TAG, "failed to start LVGL tick timer");
    return ESP_OK;
}

void DisplayManager::deinit()
{
    stopLvglTick();

    if (display_) {
        lv_display_delete(display_);
        display_ = nullptr;
    }

    if (touch_handle_) {
        esp_lcd_touch_del(touch_handle_);
        touch_handle_ = nullptr;
    }
}

esp_err_t DisplayManager::panelHandle(esp_lcd_panel_handle_t* out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (panel_handle_) {
        *out = panel_handle_;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Fetching panel handle from BSP");
    bsp_display_config_t config = {};
    ESP_RETURN_ON_ERROR(bsp_display_new(&config, &panel_handle_, nullptr), TAG, "Failed to get panel handle");
    *out = panel_handle_;
    return ESP_OK;
}

esp_err_t DisplayManager::initLvglDisplay()
{
    ESP_LOGI(TAG, "Initialising LVGL core...");
    lv_init();

#if CONFIG_LV_USE_LOG
    lv_log_register_print_cb(wavex_lvgl_log_cb);
#endif

    ESP_LOGI(TAG, "Memory before display init: free=%zu bytes, minimum=%zu bytes",
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    ESP_LOGI(TAG, "Starting BSP display with optimised configuration...");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 720 * 20,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
        }
    };

    display_ = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(display_, ESP_FAIL, TAG, "Failed to start BSP display");

    LV_LOCK();
    lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90);
    LV_UNLOCK();

    return ESP_OK;
}

esp_err_t DisplayManager::initTouchController()
{
    ESP_LOGI(TAG, "Initialising GT911 touch controller");

    i2c_master_bus_handle_t i2c_handle = nullptr;
    
    // Try to get I2C handle from BSP first
    i2c_handle = bsp_i2c_get_handle();
    if (i2c_handle == nullptr) {
        ESP_LOGI(TAG, "BSP I2C not initialised; initialising now");
        esp_err_t bsp_ret = bsp_i2c_init();
        if (bsp_ret != ESP_OK) {
            ESP_LOGW(TAG, "BSP I2C init failed (%s), creating manual I2C bus", esp_err_to_name(bsp_ret));
            
            // Manually create I2C master bus as fallback
            i2c_master_bus_config_t bus_config = {
                .i2c_port = I2C_NUM_0,
                .sda_io_num = (gpio_num_t)WAVEX_ESP_I2C_SDA,
                .scl_io_num = (gpio_num_t)WAVEX_ESP_I2C_SCL,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .intr_priority = 0,
                .trans_queue_depth = 0,
                .flags = {
                    .enable_internal_pullup = true,
                }
            };
            esp_err_t manual_ret = i2c_new_master_bus(&bus_config, &i2c_handle);
            if (manual_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create manual I2C master bus: %s", esp_err_to_name(manual_ret));
                return manual_ret;
            }
            ESP_LOGI(TAG, "Manual I2C master bus created successfully on port I2C_NUM_0");
        } else {
            i2c_handle = bsp_i2c_get_handle();
            if (i2c_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to get I2C handle after BSP initialization");
                return ESP_ERR_INVALID_STATE;
            }
            ESP_LOGI(TAG, "BSP I2C initialised successfully");
        }
    } else {
        ESP_LOGI(TAG, "Using existing BSP I2C handle");
    }
    i2c_bus_handle_ = i2c_handle;
    ESP_LOGI(TAG, "Using I2C handle with address 0x%p", (void*)i2c_handle);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << WAVEX_ESP_TOUCH_RST) | (1ULL << WAVEX_ESP_TOUCH_INT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure touch GPIO pins");

    gpio_set_level((gpio_num_t)WAVEX_ESP_TOUCH_INT, 1);
    gpio_set_level((gpio_num_t)WAVEX_ESP_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)WAVEX_ESP_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));

    io_conf.pin_bit_mask = (1ULL << WAVEX_ESP_TOUCH_INT);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to reconfigure INT pin to input");
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .scl_speed_hz = WAVEX_ESP_I2C_SPEED_HZ,
    };

    ESP_LOGI(TAG, "Creating I2C panel IO with device address 0x%02X at speed %lu Hz", 
             ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, WAVEX_ESP_I2C_SPEED_HZ);
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    
    esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_handle, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C panel IO: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C panel IO created successfully");

    esp_lcd_touch_config_t touch_config = {
        .x_max = 800,
        .y_max = 480,
        .rst_gpio_num = (gpio_num_t)WAVEX_ESP_TOUCH_RST,
        .int_gpio_num = (gpio_num_t)WAVEX_ESP_TOUCH_INT,
    };

    ret = esp_lcd_touch_new_i2c_gt911(io_handle, &touch_config, &touch_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create GT911 touch controller: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "GT911 touch controller initialised successfully");
    return ESP_OK;
}

esp_err_t DisplayManager::startLvglTick()
{
    if (lvgl_tick_timer_handle_) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick"
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &lvgl_tick_timer_handle_), TAG,
                        "Failed to create LVGL tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(lvgl_tick_timer_handle_, LV_TICK_PERIOD_MS * 1000), TAG,
                        "Failed to start LVGL tick timer");
    return ESP_OK;
}

void DisplayManager::stopLvglTick()
{
    if (!lvgl_tick_timer_handle_) {
        return;
    }

    esp_timer_stop(lvgl_tick_timer_handle_);
    esp_timer_delete(lvgl_tick_timer_handle_);
    lvgl_tick_timer_handle_ = nullptr;
}

} // namespace wavex_ui

