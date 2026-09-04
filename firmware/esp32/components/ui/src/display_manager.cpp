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
#if CONFIG_LV_USE_PPA
#include "esp_cache.h"
// lv_draw_buf_handlers_t is opaque in the public header; the struct we
// need to poke one callback on lives behind lvgl_private.h.
#include "lvgl.h"
#include "lvgl_private.h"
#endif
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

#if CONFIG_LV_USE_PPA
// Narrowed cache invalidation for the PPA draw unit.
//
// Enabling CONFIG_LV_USE_PPA does more than add a draw unit: lv_draw_ppa_init()
// calls lv_draw_buf_ppa_init_handlers(), which overrides LVGL's *global*
// invalidate_cache_cb. LVGL's own default for that callback is NULL, and
// lv_draw_buf_invalidate_cache() early-returns on NULL - so before the PPA,
// cache invalidation cost nothing at all. The PPA's replacement
// (lv_draw_ppa_buf.c) esp_cache_msync's draw_buf->data_size, the WHOLE buffer,
// ignoring the lv_area_t it is handed, and ppa_execute_drawing() calls it twice
// per draw task.
//
// Measured on the Diagnostics page (docs/performance_monitoring.md Part 2): the
// PPA cut the render p95 from 37 ms to 21 ms - it does accelerate what it
// claims to - while refr went 3->8 ms, flush 1->3 ms and CPU 18.5%->23%. Flush
// tripling is the tell, because a draw unit cannot make flushing slower; that
// cost is the global sync landing outside render. The hardware was winning and
// the callback was losing by more.
//
// So: same sync, but only over the rows the area actually covers. Correctness
// rests on it being a superset of the dirty region - we round out to cache-line
// boundaries and clamp to the buffer, and the direction is cache-to-memory
// (writeback), where flushing extra already-clean lines is harmless.
void wavexInvalidateCacheArea(const lv_draw_buf_t* draw_buf, const lv_area_t* area) {
    if (draw_buf == nullptr || draw_buf->data == nullptr || area == nullptr)
        return;

    const uint32_t stride = draw_buf->header.stride;
    const uint32_t data_size = draw_buf->data_size;
    if (stride == 0 || data_size == 0)
        return;

    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t height = lv_area_get_height(area);
    if (height <= 0)
        return;

    uint32_t begin = static_cast<uint32_t>(y1) * stride;
    if (begin >= data_size)
        return;
    uint32_t end = begin + static_cast<uint32_t>(height) * stride;
    if (end > data_size)
        end = data_size;

    // esp_cache_msync requires cache-line alignment. draw_buf->data is already
    // 128-byte aligned (CONFIG_LV_DRAW_BUF_ALIGN, which LV_USE_PPA forces to
    // equal the line size), so aligning the offsets keeps the address aligned.
    constexpr uint32_t kLine = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    begin &= ~(kLine - 1);
    end = (end + kLine - 1) & ~(kLine - 1);
    if (end > data_size)
        end = data_size;
    if (end <= begin)
        return;

    esp_err_t err = esp_cache_msync(draw_buf->data + begin,
                                    end - begin,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    // Once, not per frame: this runs on every draw task and a repeating log
    // would cost more than the sync it is complaining about.
    static bool reported = false;
    if (err != ESP_OK && !reported) {
        reported = true;
        ESP_LOGW(TAG,
                 "esp_cache_msync failed (%s); PPA cache sync may be a no-op",
                 esp_err_to_name(err));
    }
}
#endif  // CONFIG_LV_USE_PPA

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

uint32_t DisplayManager::nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

esp_err_t DisplayManager::setBrightness(uint8_t percent) {
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = bsp_display_brightness_set(percent);
    if (err == ESP_OK && percent > 0) {
        brightness_percent_ = percent;
    }
    return err;
}

void DisplayManager::noteUserActivity() {
    activity_ms_.store(nowMs(), std::memory_order_release);
}

void DisplayManager::serviceScreenBlanker() {
    const uint32_t now_ms = nowMs();
    const uint32_t activity_ms = activity_ms_.load(std::memory_order_acquire);

    // Only the UI task writes the backlight. Touch arrives on the LVGL task,
    // so its callback records a timestamp and this service pass performs the
    // bounded I2C transaction without adding work to the render task.
    if (screen_blanker_.blanked()) {
        if (activity_ms != 0 && activity_ms != last_observed_activity_ms_ &&
            screen_blanker_.ShouldWake(activity_ms)) {
            last_observed_activity_ms_ = activity_ms;
            if (esp_err_t err = setBrightness(brightness_percent_); err != ESP_OK) {
                ESP_LOGW(TAG, "screen wake backlight restore failed (%d)", err);
            }
        }
        return;
    }

    // Record activity after testing the blank state. Repeated timestamps are
    // harmless; the ScreenBlanker owns the timeout arithmetic.
    if (activity_ms != 0 && activity_ms != last_observed_activity_ms_) {
        screen_blanker_.RecordActivity(activity_ms);
        last_observed_activity_ms_ = activity_ms;
    }
    if (screen_blanker_.ShouldBlank(now_ms)) {
        if (esp_err_t err = bsp_display_brightness_set(0); err != ESP_OK) {
            ESP_LOGW(TAG, "screen blank backlight disable failed (%d)", err);
            // A failed write must not leave the policy thinking the panel is
            // safely blanked; the next pass should retry.
            screen_blanker_.ShouldWake(now_ms);
        }
    }
}

void DisplayManager::touchActivityEventCb(lv_event_t* event) {
    auto* manager = static_cast<DisplayManager*>(lv_event_get_user_data(event));
    if (manager) {
        manager->noteUserActivity();
    }
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

#if CONFIG_LV_USE_PPA
    // Must come after the port's lv_init(): lv_draw_ppa_init() installs the
    // whole-buffer callback we are replacing, so registering earlier would just
    // be overwritten. See wavexInvalidateCacheArea above for the measurements.
    lv_draw_buf_get_handlers()->invalidate_cache_cb = wavexInvalidateCacheArea;
    ESP_LOGI(TAG, "LVGL cache invalidation narrowed to the dirty area");
#endif

    LV_LOCK();
    lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90);
    LV_UNLOCK();

    // Drive the backlight to a known level. bsp_display_new() only *inits* the
    // brightness path (an I2C bus handle); it never sets a level, so without
    // this the panel sits at whatever the backlight driver powers up at and
    // Settings > Display would open showing a number that is not the truth.
    // The BSP offers no getter, so a known write is the only way to make the
    // UI's model and the hardware agree.
    if (esp_err_t err = setBrightness(100); err != ESP_OK) {
        ESP_LOGW(TAG, "Backlight set to 100%% failed (%d); brightness readout may be wrong", err);
    }

    screen_blanker_.Init(nowMs());
    const uint32_t activity_ms = nowMs();
    activity_ms_.store(activity_ms, std::memory_order_release);
    last_observed_activity_ms_ = activity_ms;
    if (lv_indev_t* touch = bsp_display_get_input_dev()) {
        // Input-device events see every touch before it reaches page widgets;
        // page-local click callbacks would miss drags and empty screen areas.
        lv_indev_add_event_cb(touch, touchActivityEventCb, LV_EVENT_PRESSED, this);
    } else {
        ESP_LOGW(TAG, "No LVGL touch input device; touch cannot wake screen blanking");
    }

    return ESP_OK;
}

}  // namespace wavex_ui
