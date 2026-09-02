/**
 * @file ui_task.cpp
 * @brief UI Task Implementation for MIPI DSI Display with LVGL
 *
 * This implementation provides full LVGL integration with MIPI DSI display
 * using the Waveshare 5-DSI-TOUCH-A display and HX8394 driver.
 */

#include "ui_task.h"

#include <stdlib.h>

#include "comm/statistics.h"
#include "config/hardware_config.h"
#include "config/pin_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portable.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "links/esp_spi_link.h"
#include "pcnt_task.h"
#include "ui/ui_api.h"
#include "ui/ui_busy_overlay.h"
#include "ui/ui_sample_browser.h"
#include "ui/ui_screenshot.h"

#include <atomic>

// LVGL includes
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui/display_manager.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_navigation_integration.h"

// LVGL port lock macros for thread safety
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

// Include BSP header for display functions
#include "bsp/esp32_p4_nano.h"
#include "ui/tca8418_keypad.h"

static const char *TAG = "UI_TASK";

// Adaptive refresh rate control constants
static const uint32_t MIN_REFRESH_INTERVAL_MS = 16;   // 60 FPS maximum
static const uint32_t MAX_REFRESH_INTERVAL_MS = 100;  // 10 FPS minimum

// Global UITask instance (singleton pattern)
static UITask *g_ui_task_instance = nullptr;

// Shutdown handshake. The UI task takes the LVGL port lock, so deleting it
// outright could leave that lock held forever and wedge the LVGL port task.
static std::atomic<bool> s_ui_running{false};

// UITask class implementation
UITask::UITask(WaveX::Comm::ICommInterface &comm_interface) : m_comm_interface(comm_interface) {
    m_context.comm_interface = &m_comm_interface;

    // Register comm interface with UI system for page creation
    wavex_ui::ui_set_comm_interface(&m_comm_interface);

    ESP_LOGI(TAG, "UITask created with injected CommInterface");
}

esp_err_t UITask::init() {
    ESP_LOGI(TAG, "Initializing UITask");

    ESP_LOGI(TAG, "Initializing LVGL display...");
    esp_err_t lvgl_ret = wavex_ui::DisplayManager::instance().init();
    if (lvgl_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL display: %s", esp_err_to_name(lvgl_ret));
        return lvgl_ret;
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    ESP_LOGI(TAG, "Memory after display init:");
    ESP_LOGI(TAG, "  Free heap: %zu bytes", esp_get_free_heap_size());

    return ESP_OK;
}

esp_err_t UITask::start() {
    ESP_LOGI(TAG, "Starting UITask");

    // No meter listener is registered here. The meters live in the header
    // status strip, which drives itself from an lv_timer and reads
    // inter_mcu_get_meter_data(); this class's parallel meter pipeline was
    // never reachable and is gone.

    // Start TCA8418 keypad on the BSP I2C bus; pin and address from config.
    {
        const int tca_int_gpio = WAVEX_ESP_BTN_INT;
        const uint8_t tca_addr = WAVEX_TCA8418_I2C_ADDR;
        esp_err_t kret = wavex_ui::tca8418_keypad_start(tca_int_gpio, tca_addr);
        if (kret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start TCA8418 keypad: %s", esp_err_to_name(kret));
        } else {
            ESP_LOGI(
                TAG, "TCA8418 keypad started (INT GPIO=%d, addr=0x%02X)", tca_int_gpio, tca_addr);
        }
    }

    wavex_ui::initNavigationSystem();

    wavex_ui::InputDispatcher::instance().setActiveContext(wavex_ui::createNavigationContext());

    s_ui_running = true;
    TaskHandle_t handle = NULL;
    BaseType_t task_ret =
        xTaskCreatePinnedToCore(uiTaskFunction,
                                "ui_task",
                                16384,  // Increased stack size for LVGL and diagnostics
                                this,   // Pass this instance as parameter
                                2,      // Priority
                                &handle,
                                1  // Run on Core 1
        );
    m_context.ui_task_handle = handle;

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        s_ui_running = false;
        return ESP_FAIL;
    }

    g_ui_task_instance = this;

    ESP_LOGI(TAG, "UITask started successfully");
    return ESP_OK;
}

esp_err_t UITask::stop() {
    ESP_LOGI(TAG, "Stopping UITask");

    // Ask the task to leave rather than deleting it. Its loop takes the LVGL
    // port lock (input dispatch, deferred updates, lv_refr_now), and killing
    // it inside one of those regions would leave the lock held forever and
    // wedge the LVGL port task with it. It exits between passes, always
    // unlocked, and clears its own handle.
    s_ui_running = false;
    for (int waited_ms = 0; m_context.ui_task_handle != NULL && waited_ms < 500; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (m_context.ui_task_handle != NULL) {
        ESP_LOGE(TAG, "UI task did not exit; leaving the display up");
        return ESP_ERR_TIMEOUT;
    }

    wavex_ui::DisplayManager::instance().deinit();

    ESP_LOGI(TAG, "UITask stopped");
    return ESP_OK;
}

void UITask::markContentChanged() {
    m_context.content_changed = true;
}

esp_err_t UITask::getPanelHandle(esp_lcd_panel_handle_t *panel_handle) {
    return wavex_ui::DisplayManager::instance().panelHandle(panel_handle);
}

void UITask::adaptiveRefreshControl() {
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);  // Convert to ms

    if (m_context.content_changed) {
        uint32_t time_since_last_refresh = current_time - m_context.last_refresh_time;

        if (time_since_last_refresh >= MIN_REFRESH_INTERVAL_MS) {
            if (auto *display = wavex_ui::DisplayManager::instance().display()) {
                LV_LOCK();
                lv_refr_now(display);
                LV_UNLOCK();
            }

            m_context.content_changed = false;
            m_context.last_refresh_time = current_time;
            m_context.refresh_count++;

            if (m_context.refresh_count % 100 == 0) {
                ESP_LOGD(TAG, "Display refresh count: %lu", m_context.refresh_count);
            }
        }
    }
}

void UITask::uiTaskFunction(void *pvParameters) {
    UITask *ui_task = (UITask *)pvParameters;
    if (!ui_task) {
        ESP_LOGE(TAG, "UI task function called with NULL parameter");
        vTaskDelete(NULL);
        return;
    }

    ui_task->run();
}

void UITask::run() {
    ESP_LOGI(TAG, "UI task started with full UI support");

    ESP_LOGI(TAG, "Handing layout control to navigator stack");

    ESP_LOGI(TAG, "UI loop started with adaptive refresh rate control");
    while (s_ui_running) {
        int32_t enc_delta = pcnt_consume_delta(WAVEX_ENCODER_PCNT_UNIT);
        if (enc_delta != 0) {
            wavex_ui::InputEvent evt;
            evt.type = (enc_delta > 0) ? wavex_ui::InputType::EncoderRight
                                       : wavex_ui::InputType::EncoderLeft;
            // Magnitude only - the type above carries the direction. Posting
            // the raw signed count here is what let a page negating `delta` on
            // an EncoderLeft come out positive, which is how counter-clockwise
            // ended up increasing values. The pot below has always posted a
            // magnitude; these two now agree. See InputEvent::steps().
            evt.delta = (int16_t)((enc_delta > 0) ? enc_delta : -enc_delta);
            evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            wavex_ui::InputDispatcher::instance().post(evt);

            // Navigation system handles all encoder input through InputDispatcher
            // No need for manual encoder handling here
        }

#if WAVEX_ESP_PCNT1_ENABLED
        int32_t pot_delta = pcnt_consume_delta(WAVEX_PCNT1_UNIT);
        if (pot_delta != 0) {
            // Accumulate raw counts and consume whole detents, carrying the
            // remainder forward. The old reset-to-zero logic discarded every
            // count beyond one detent per poll window, so fast turns lost
            // most of their steps - the list moved far less than the knob.
            m_context.pcnt1_delta_accumulator += pot_delta;

            const int32_t detents =
                m_context.pcnt1_delta_accumulator / WAVEX_PCNT1_COUNTS_PER_DETENT;
            if (detents != 0) {
                m_context.pcnt1_delta_accumulator -= detents * WAVEX_PCNT1_COUNTS_PER_DETENT;

                // One event carrying the full detent count; onInput walks the
                // delta one entry at a time.
                wavex_ui::InputEvent pot_evt;
                pot_evt.type = (detents > 0) ? wavex_ui::InputType::EncoderUp
                                             : wavex_ui::InputType::EncoderDown;
                pot_evt.delta = (int16_t)((detents > 0) ? detents : -detents);
                pot_evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

                ESP_LOGD(TAG,
                         "PCNT1 encoder: %ld detent(s), posting %s (remainder=%d)",
                         (long)detents,
                         (detents > 0) ? "EncoderUp" : "EncoderDown",
                         m_context.pcnt1_delta_accumulator);

                bool posted = wavex_ui::InputDispatcher::instance().post(pot_evt);
                if (!posted) {
                    ESP_LOGW(TAG, "Failed to post PCNT1 encoder event to queue");
                }
            }
        }
#endif
        // Dispatch queued input events to current context. processAll() takes
        // the LVGL port lock around each event itself (see input_dispatcher.cpp
        // for why per event and not around the drain), so no lock here.
        wavex_ui::InputDispatcher::instance().processAll();

        // Debug-build serial screenshots (no-op stub in release; manages
        // its own LVGL locking, so called outside LV_LOCK).
        wavex_screenshot_poll();

        // Apply what the comm callbacks staged. Those run on the UART task and
        // may only raise flags; this is where the widgets actually change.
        LV_LOCK();
        wavex_ui::BusyOverlay::service();
        wavex_ui::UISampleBrowser::processDeferredUpdates();
        LV_UNLOCK();

        adaptiveRefreshControl();

        vTaskDelay(pdMS_TO_TICKS(32));  // 32ms delay for 30 FPS theoretical maximum
    }

    // Reached only via stop(). Published before self-deleting, and always
    // outside LV_LOCK, so the port lock cannot be left held.
    ESP_LOGI(TAG, "UI task exiting");
    m_context.ui_task_handle = NULL;
    vTaskDelete(NULL);
}

// Global functions for C compatibility
esp_err_t wavex_ui_task_start(WaveX::Comm::ICommInterface &comm_interface) {
    if (g_ui_task_instance) {
        ESP_LOGE(TAG, "UI task already started");
        return ESP_FAIL;
    }

    g_ui_task_instance = new UITask(comm_interface);
    if (!g_ui_task_instance) {
        ESP_LOGE(TAG, "Failed to create UI task instance");
        return ESP_FAIL;
    }

    esp_err_t ret = g_ui_task_instance->init();
    if (ret != ESP_OK) {
        delete g_ui_task_instance;
        g_ui_task_instance = nullptr;
        return ret;
    }

    return g_ui_task_instance->start();
}

esp_err_t wavex_ui_task_stop(void) {
    if (!g_ui_task_instance) {
        ESP_LOGW(TAG, "UI task not started");
        return ESP_OK;
    }

    esp_err_t ret = g_ui_task_instance->stop();
    delete g_ui_task_instance;
    g_ui_task_instance = nullptr;
    return ret;
}

esp_err_t wavex_ui_get_panel_handle(esp_lcd_panel_handle_t *panel_handle) {
    if (!g_ui_task_instance) {
        ESP_LOGE(TAG, "UI task not started");
        return ESP_FAIL;
    }

    return g_ui_task_instance->getPanelHandle(panel_handle);
}

void wavex_ui_mark_content_changed(void) {
    if (g_ui_task_instance) {
        g_ui_task_instance->markContentChanged();
    }
}
