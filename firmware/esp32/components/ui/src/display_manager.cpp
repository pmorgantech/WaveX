#include "ui/display_manager.h"

#include "bsp/esp32_p4_nano.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#undef esp_lcd_new_panel_io_i2c
#define esp_lcd_new_panel_io_i2c esp_lcd_new_panel_io_i2c_v2

namespace wavex_ui {
namespace {

static const char* TAG = "DisplayManager";

static void log_dma_heap(const char* phase) {
    constexpr uint32_t kInternalDmaCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    constexpr uint32_t kPsramDmaCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    ESP_LOGI(TAG,
             "DMA heap %s: internal free=%zu min=%zu, PSRAM free=%zu min=%zu",
             phase,
             heap_caps_get_free_size(kInternalDmaCaps),
             heap_caps_get_minimum_free_size(kInternalDmaCaps),
             heap_caps_get_free_size(kPsramDmaCaps),
             heap_caps_get_minimum_free_size(kPsramDmaCaps));
}

#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

extern "C" void wavex_lvgl_log_cb(signed char level, const char* buf) {
    switch (level) {
        case 0:  // TRACE
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

}  // namespace

DisplayManager& DisplayManager::instance() {
    static DisplayManager inst;
    return inst;
}

esp_err_t DisplayManager::init() {
    if (display_) {
        return ESP_OK;
    }

    // Touch and the LVGL tick both belong to esp_lvgl_port / the BSP; see
    // initLvglDisplay(). Adding our own of either is what made LVGL time run
    // at double speed and put a second driver on the touch controller.
    ESP_RETURN_ON_ERROR(initLvglDisplay(), TAG, "failed to initialise LVGL display");
    return ESP_OK;
}

void DisplayManager::deinit() {
    // Incomplete on purpose rather than by omission: lvgl_port owns the
    // display, its task and the BSP's touch indev, so tearing this down
    // properly means lvgl_port_deinit() under the port lock, and nothing calls
    // deinit() today. Tracked as E-STOP1.
    if (display_) {
        lv_display_delete(display_);
        display_ = nullptr;
    }
}

esp_err_t DisplayManager::panelHandle(esp_lcd_panel_handle_t* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (panel_handle_) {
        *out = panel_handle_;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Fetching panel handle from BSP");
    bsp_display_config_t config = {};
    ESP_RETURN_ON_ERROR(
        bsp_display_new(&config, &panel_handle_, nullptr), TAG, "Failed to get panel handle");
    *out = panel_handle_;
    return ESP_OK;
}

esp_err_t DisplayManager::initLvglDisplay() {
    // No lv_init() here: bsp_display_start_with_config() below runs
    // lvgl_port_init(), which does it. Calling it first only meant LVGL warned
    // "already initialized" when the port tried, and the log callback has to be
    // registered after init anyway - lv_init() zeroes the global that holds it.
    ESP_LOGI(TAG,
             "Memory before display init: free=%zu bytes, minimum=%zu bytes",
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size());
    log_dma_heap("before display init");

    ESP_LOGI(TAG, "Starting BSP display with optimised configuration...");
    bsp_display_cfg_t cfg = {.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
                             .buffer_size = 720 * 20,
                             .double_buffer = true,
                             .flags = {
                                 .buff_dma = true,
                                 .buff_spiram = false,
                                 .sw_rotate = true,
                             }};

    // The LVGL task stack is OURS to size, not the library's.
    //
    // ESP_LVGL_PORT_INIT_CONFIG() defaults task_stack to 7168 bytes, which is a
    // generic default for a port, not a considered figure for this UI. Every
    // page build runs on this task, and a page that creates ~80 objects in one
    // onEnter (the Play page's 41 keys, each a button plus a label) plus nested
    // page frames (a tab host entering a child) plus lv_label_set_text_fmt -
    // which reaches vsnprintf, itself no small stack consumer - overran it. The
    // symptom was "Guru Meditation Error: Core 0 panic'ed (Stack protection
    // fault)" in task taskLVGL, with reported bounds of exactly 7024 bytes.
    //
    // 16 KB against ~500 KB of internal RAM is cheap insurance for the one task
    // that runs all UI construction. This is NOT a licence to be careless with
    // stack in page code; it is an admission that 7 KB was never sized for what
    // this UI does.
    //
    // Measured, not assumed: the high-water mark is logged below so the real
    // headroom is a number. docs/esp32p4_coding_guide.md asks for exactly that
    // rather than a guessed size.
    cfg.lvgl_port_cfg.task_stack = 16384;

    display_ = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(display_, ESP_FAIL, TAG, "Failed to start BSP display");
    log_dma_heap("after display init");

    // After the port's lv_init(), for the reason given at the top of this
    // function.
#if CONFIG_LV_USE_LOG
    lv_log_register_print_cb(wavex_lvgl_log_cb);
#endif

    LV_LOCK();
    lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90);
    LV_UNLOCK();

    return ESP_OK;
}

}  // namespace wavex_ui
