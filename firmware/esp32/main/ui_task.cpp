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
#include "pages/diagnostics_page.h"
#include "pcnt_task.h"
#include "ui/ui_sample_browser.h"

// LVGL includes
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui/display_manager.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_demo.h"
#include "ui/ui_navigation_integration.h"

// LVGL port lock macros for thread safety
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

// Include BSP header for display functions
#include "bsp/esp32_p4_nano.h"
#include "ui/tca8418_keypad.h"

static const char *TAG = "UI_TASK";

// Peak hold tracking timeout
static const uint32_t PEAK_HOLD_TIMEOUT_MS = 500;  // Configurable peak hold duration

// Adaptive refresh rate control constants
static const uint32_t MIN_REFRESH_INTERVAL_MS = 16;   // 60 FPS maximum
static const uint32_t MAX_REFRESH_INTERVAL_MS = 100;  // 10 FPS minimum

// Global UITask instance (singleton pattern)
UITask *g_ui_task_instance = nullptr;

// UITask class implementation
UITask::UITask(WaveX::Comm::ICommInterface &comm_interface) : m_comm_interface(comm_interface) {
    // Initialize the UI context with injected dependencies
    m_context.comm_interface = &m_comm_interface;
    ESP_LOGI(TAG, "UITask created with injected CommInterface at %p", &comm_interface);
}

esp_err_t UITask::init() {
    ESP_LOGI(TAG, "Initializing UITask");

    // Initialize LVGL display
    ESP_LOGI(TAG, "Initializing LVGL display...");
    esp_err_t lvgl_ret = wavex_ui::DisplayManager::instance().init();
    if (lvgl_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL display: %s", esp_err_to_name(lvgl_ret));
        return lvgl_ret;
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    // Log memory status
    ESP_LOGI(TAG, "Memory after display init:");
    ESP_LOGI(TAG, "  Free heap: %zu bytes", esp_get_free_heap_size());

    return ESP_OK;
}

esp_err_t UITask::start() {
    ESP_LOGI(TAG, "Starting UITask");

    // Register meter data callback for real-time audio meter updates
    m_context.comm_interface->setMeterListener(meterDataCallback, &m_context);
    ESP_LOGI(TAG, "Meter data callback registered via interface");

    // Test meter callback with dummy stereo data to verify it's working
    ESP_LOGI(TAG, "Testing meter callback with dummy stereo data...");
    meterDataCallback(0.5f, 0.4f, 0.8f, 0.7f, &m_context);  // Test with 50%/40% RMS, 80%/70% peak

    // Start TCA8418 keypad on BSP I2C; INT on GPIO31 per pin_config
    {
        const int tca_int_gpio = WAVEX_ESP_BTN_INT;  // GPIO31
        const uint8_t tca_addr = 0x34;
        esp_err_t kret = wavex_ui::tca8418_keypad_start(tca_int_gpio, tca_addr);
        if (kret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start TCA8418 keypad: %s", esp_err_to_name(kret));
        } else {
            ESP_LOGI(
                TAG, "TCA8418 keypad started (INT GPIO=%d, addr=0x%02X)", tca_int_gpio, tca_addr);
        }
    }

    // Initialize navigation system
    wavex_ui::initNavigationSystem();

    // Set navigation context as active input handler
    wavex_ui::InputDispatcher::instance().setActiveContext(wavex_ui::createNavigationContext());

    // Create and start meter update timer
    const esp_timer_create_args_t meter_timer_args = {.callback = &meterUpdateCallback,
                                                      .arg = &m_context,
                                                      .dispatch_method = ESP_TIMER_TASK,
                                                      .name = "meter_timer",
                                                      .skip_unhandled_events = false};
    esp_err_t timer_ret = esp_timer_create(&meter_timer_args, &m_context.meter_timer_handle);
    if (timer_ret == ESP_OK) {
        esp_timer_start_periodic(m_context.meter_timer_handle, 33000); // 33ms in microseconds (30 FPS)
        ESP_LOGI(TAG, "Meter timer started (33ms real-time updates - 30 FPS)");
    } else {
        ESP_LOGE(TAG, "Failed to create meter timer: %s", esp_err_to_name(timer_ret));
        return timer_ret;
    }

    // Create UI task
    BaseType_t task_ret =
        xTaskCreatePinnedToCore(uiTaskFunction,
                                "ui_task",
                                16384,  // Increased stack size for LVGL and diagnostics
                                this,   // Pass this instance as parameter
                                2,      // Priority
                                &m_context.ui_task_handle,
                                1  // Run on Core 1
        );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_FAIL;
    }

    // Set global instance
    g_ui_task_instance = this;

    ESP_LOGI(TAG, "UITask started successfully");
    return ESP_OK;
}

esp_err_t UITask::stop() {
    ESP_LOGI(TAG, "Stopping UITask");

    if (m_context.ui_task_handle != NULL) {
        vTaskDelete(m_context.ui_task_handle);
        m_context.ui_task_handle = NULL;
    }

    // Stop diagnostics page module
    diagnostics_page_stop();

    // Stop and delete the meter timer
    if (m_context.meter_timer_handle) {
        esp_timer_stop(m_context.meter_timer_handle);
        esp_timer_delete(m_context.meter_timer_handle);
        m_context.meter_timer_handle = NULL;
    }

    // Delete LVGL timer
    if (m_context.meter_lvgl_timer) {
        lv_timer_delete(m_context.meter_lvgl_timer);
        m_context.meter_lvgl_timer = NULL;
    }

    // Let display manager clean up LVGL and touch resources
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

// Static callback functions (need to access UITask instance)
void UITask::meterUpdateCallback(void *arg) {
    UiContext *ctx = (UiContext *)arg;
    if (!ctx)
        return;

    uint32_t current_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool should_reset_meters = false;

    // Check if we have valid callback data and if it's recent (within 500ms)
    if (ctx->meter_callback_data_valid) {
        uint32_t time_since_callback = current_time_ms - ctx->last_callback_time_ms;
        if (time_since_callback > 500) {
            // Callback data is stale, reset meters
            should_reset_meters = true;
            ctx->meter_callback_data_valid = false;
        }
    } else {
        // No callback data received yet, reset meters (normal when no audio is running)
        should_reset_meters = true;
    }

    if (should_reset_meters) {
        // Only reset if we haven't already reset recently (avoid continuous updates)
        static uint32_t last_reset_time = 0;
        uint32_t time_since_reset = current_time_ms - last_reset_time;

        if (time_since_reset > 1000) {  // Only reset once per second
            // Reset peak hold values to 0
            ctx->meter_display.peak_hold_l.peak_value = 0.0f;
            ctx->meter_display.peak_hold_l.peak_time_ms = current_time_ms;
            ctx->meter_display.peak_hold_l.is_holding = false;

            ctx->meter_display.peak_hold_r.peak_value = 0.0f;
            ctx->meter_display.peak_hold_r.peak_time_ms = current_time_ms;
            ctx->meter_display.peak_hold_r.is_holding = false;

            last_reset_time = current_time_ms;

            // Mark meter reset as pending (will be processed by main UI task)
            ctx->meter_reset_pending = true;
            if (g_ui_task_instance) {
                g_ui_task_instance->markContentChanged();
            }
        }
    } else if (ctx->meter_callback_data_valid) {
        // Normal operation - update meters with valid callback data
        // Update peak hold tracking
        updatePeakHoldL(ctx->meter_display.peak_hold_l, ctx->current_peak_left, current_time_ms);
        updatePeakHoldR(ctx->meter_display.peak_hold_r, ctx->current_peak_right, current_time_ms);

        // Store meter data for deferred update (no LVGL locks in timer callback)
        ctx->deferred_rms_left = ctx->current_rms_left;
        ctx->deferred_rms_right = ctx->current_rms_right;
        ctx->deferred_peak_left = ctx->meter_display.peak_hold_l.peak_value;
        ctx->deferred_peak_right = ctx->meter_display.peak_hold_r.peak_value;

        // Mark meter update as pending (will be processed by main UI task)
        ctx->meter_update_pending = true;
        if (g_ui_task_instance) {
            g_ui_task_instance->markContentChanged();
        }
    }
}

void UITask::lvglMeterApplyCallback(lv_timer_t *timer) {
    UiContext *ctx = (UiContext *)lv_timer_get_user_data(timer);
    if (!ctx)
        return;

    if (!ctx->meter_update_pending && !ctx->meter_reset_pending)
        return;

    // Reset handling
    if (ctx->meter_reset_pending) {
        if (ctx->meter_display.bar_l && ctx->meter_display.bar_r &&
            ctx->meter_display.peak_line_l && ctx->meter_display.peak_line_r &&
            lv_obj_is_valid(ctx->meter_display.bar_l) &&
            lv_obj_is_valid(ctx->meter_display.bar_r) &&
            lv_obj_is_valid(ctx->meter_display.peak_line_l) &&
            lv_obj_is_valid(ctx->meter_display.peak_line_r)) {
            lv_bar_set_value(ctx->meter_display.bar_l, 0, LV_ANIM_ON);
            lv_bar_set_value(ctx->meter_display.bar_r, 0, LV_ANIM_ON);
            updatePeakLinePosition(ctx->meter_display.peak_line_l, ctx->meter_display.bar_l, 0.0f);
            updatePeakLinePosition(ctx->meter_display.peak_line_r, ctx->meter_display.bar_r, 0.0f);
            if (ctx->meter_display.label_l && lv_obj_is_valid(ctx->meter_display.label_l))
                lv_label_set_text(ctx->meter_display.label_l, "L: 0.000\nPeak: 0.000");
            if (ctx->meter_display.label_r && lv_obj_is_valid(ctx->meter_display.label_r))
                lv_label_set_text(ctx->meter_display.label_r, "R: 0.000\nPeak: 0.000");
        }
        ctx->meter_reset_pending = false;
        ctx->meter_update_pending = false;
        return;
    }

    if (ctx->meter_update_pending) {
        if (ctx->meter_display.bar_l && ctx->meter_display.bar_r &&
            ctx->meter_display.peak_line_l && ctx->meter_display.peak_line_r &&
            lv_obj_is_valid(ctx->meter_display.bar_l) &&
            lv_obj_is_valid(ctx->meter_display.bar_r) &&
            lv_obj_is_valid(ctx->meter_display.peak_line_l) &&
            lv_obj_is_valid(ctx->meter_display.peak_line_r)) {
            int rms_l_value = (int)(ctx->deferred_rms_left * 100.0f);
            int rms_r_value = (int)(ctx->deferred_rms_right * 100.0f);
            if (rms_l_value > 100)
                rms_l_value = 100;
            if (rms_r_value > 100)
                rms_r_value = 100;
            lv_bar_set_value(ctx->meter_display.bar_l, rms_l_value, LV_ANIM_ON);
            lv_bar_set_value(ctx->meter_display.bar_r, rms_r_value, LV_ANIM_ON);
            updatePeakLinePosition(
                ctx->meter_display.peak_line_l, ctx->meter_display.bar_l, ctx->deferred_peak_left);
            updatePeakLinePosition(
                ctx->meter_display.peak_line_r, ctx->meter_display.bar_r, ctx->deferred_peak_right);
            if (ctx->meter_display.label_l && lv_obj_is_valid(ctx->meter_display.label_l)) {
                char label_text[32];
                snprintf(label_text,
                         sizeof(label_text),
                         "L: %.3f\nPeak: %.3f",
                         ctx->deferred_rms_left,
                         ctx->deferred_peak_left);
                lv_label_set_text(ctx->meter_display.label_l, label_text);
            }
            if (ctx->meter_display.label_r && lv_obj_is_valid(ctx->meter_display.label_r)) {
                char label_text[32];
                snprintf(label_text,
                         sizeof(label_text),
                         "R: %.3f\nPeak: %.3f",
                         ctx->deferred_rms_right,
                         ctx->deferred_peak_right);
                lv_label_set_text(ctx->meter_display.label_r, label_text);
            }
        }
        ctx->meter_update_pending = false;
    }
}

void UITask::meterDataCallback(
    float rms_left, float rms_right, float peak_left, float peak_right, void *user_data) {
    UiContext *ctx = (UiContext *)user_data;
    if (!ctx)
        return;

// Debug logging for meter data
#ifdef WAVEX_LOG_METER_DATA
    ESP_LOGI(TAG,
             "Meter data received: RMS_L=%.3f RMS_R=%.3f Peak_L=%.3f Peak_R=%.3f",
             rms_left,
             rms_right,
             peak_left,
             peak_right);
#endif

    // Store real-time stereo meter data in context
    ctx->current_rms_left = rms_left;
    ctx->current_rms_right = rms_right;
    ctx->current_peak_left = peak_left;
    ctx->current_peak_right = peak_right;

    // Mark that we have fresh callback data available
    ctx->meter_callback_data_valid = true;
    ctx->last_callback_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

// Peak hold management
void UITask::updatePeakHoldL(PeakHoldData &peak_data,
                             float current_peak,
                             uint32_t current_time_ms) {
    if (current_peak > peak_data.peak_value) {
        // New peak detected - start holding
        peak_data.peak_value = current_peak;
        peak_data.peak_time_ms = current_time_ms;
        peak_data.is_holding = true;
    } else if (peak_data.is_holding &&
               (current_time_ms - peak_data.peak_time_ms) >= PEAK_HOLD_TIMEOUT_MS) {
        // Peak hold timeout expired - revert to current peak
        peak_data.peak_value = current_peak;
        peak_data.is_holding = false;
    }
}

void UITask::updatePeakHoldR(PeakHoldData &peak_data,
                             float current_peak,
                             uint32_t current_time_ms) {
    if (current_peak > peak_data.peak_value) {
        // New peak detected - start holding
        peak_data.peak_value = current_peak;
        peak_data.peak_time_ms = current_time_ms;
        peak_data.is_holding = true;
    } else if (peak_data.is_holding &&
               (current_time_ms - peak_data.peak_time_ms) >= PEAK_HOLD_TIMEOUT_MS) {
        // Peak hold timeout expired - revert to current peak
        peak_data.peak_value = current_peak;
        peak_data.is_holding = false;
    }
}

void UITask::updatePeakLinePosition(lv_obj_t *peak_line, lv_obj_t *meter_bar, float peak_value) {
    if (!peak_line || !meter_bar || !lv_obj_is_valid(peak_line) || !lv_obj_is_valid(meter_bar)) {
        return;
    }

    // Get meter bar dimensions (peak line is now a child of meter bar)
    lv_coord_t bar_width = lv_obj_get_width(meter_bar);
    lv_coord_t bar_height = lv_obj_get_height(meter_bar);

    // Calculate peak position (0-100% of bar width)
    int peak_percent = (int)(peak_value * 100.0f);
    if (peak_percent > 100)
        peak_percent = 100;
    if (peak_percent < 0)
        peak_percent = 0;

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

void UITask::adaptiveRefreshControl() {
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);  // Convert to ms

    // Only refresh if content has changed and enough time has passed
    if (m_context.content_changed) {
        uint32_t time_since_last_refresh = current_time - m_context.last_refresh_time;

        // Use minimum refresh interval for responsive updates
        if (time_since_last_refresh >= MIN_REFRESH_INTERVAL_MS) {
            if (auto *display = wavex_ui::DisplayManager::instance().display()) {
                LV_LOCK();
                lv_refr_now(display);
                LV_UNLOCK();
            }

            m_context.content_changed = false;
            m_context.last_refresh_time = current_time;
            m_context.refresh_count++;

            // Log refresh rate every 100 refreshes for monitoring
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

    // Defer layout construction to the navigator stack
    ESP_LOGI(TAG, "Handing layout control to navigator stack");

    // Main UI loop with adaptive refresh rate control
    ESP_LOGI(TAG, "UI loop started with adaptive refresh rate control");
    while (1) {
        // Apply encoder movement to active UI when applicable
        int32_t enc_delta = pcnt_consume_delta(WAVEX_ENCODER_PCNT_UNIT);
        if (enc_delta != 0) {
            // Post unified input events for encoder movement
            wavex_ui::InputEvent evt;
            evt.type = (enc_delta > 0) ? wavex_ui::InputType::EncoderRight
                                       : wavex_ui::InputType::EncoderLeft;
            evt.delta = (int16_t)enc_delta;
            evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            wavex_ui::InputDispatcher::instance().post(evt);

            // Navigation system handles all encoder input through InputDispatcher
            // No need for manual encoder handling here
        }

        // Read potentiometer encoder (PCNT1) for scrolling
#if WAVEX_ESP_PCNT1_ENABLED
        int32_t pot_delta = pcnt_consume_delta(WAVEX_PCNT1_UNIT);
        if (pot_delta != 0) {
            // Convert PCNT1 delta to EncoderUp/EncoderDown events
            // Negative delta (counter decreasing) = scrolling up = EncoderUp
            // Positive delta (counter increasing) = scrolling down = EncoderDown
            wavex_ui::InputEvent pot_evt;
            pot_evt.type =
                (pot_delta > 0) ? wavex_ui::InputType::EncoderDown : wavex_ui::InputType::EncoderUp;
            pot_evt.delta =
                (int16_t)(pot_delta > 0 ? pot_delta : -pot_delta);  // Use absolute value
            pot_evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGI(TAG,
                     "PCNT1 encoder: delta=%d, posting %s event",
                     pot_delta,
                     (pot_delta > 0) ? "EncoderDown" : "EncoderUp");
            bool posted = wavex_ui::InputDispatcher::instance().post(pot_evt);
            if (!posted) {
                ESP_LOGW(TAG, "Failed to post PCNT1 encoder event to queue");
            }
        }
#endif
        // Dispatch queued input events to current context
        wavex_ui::InputDispatcher::instance().processAll();

        // Process deferred diagnostics updates (prevents deadlock)
        diagnostics_page_process_deferred_updates();

        // Process deferred sample browser updates (prevents deadlock from SPI/UART task)
        // Acquire LVGL lock before processing deferred updates
        LV_LOCK();
        wavex_ui::UISampleBrowser::processDeferredUpdates();
        LV_UNLOCK();

        // Use adaptive refresh control for optimal performance
        adaptiveRefreshControl();

        // Short delay to prevent excessive CPU usage
        vTaskDelay(pdMS_TO_TICKS(32));  // 32ms delay for 30 FPS theoretical maximum
    }
}

// UITask implementation
void UITask::createMeterDisplay(lv_obj_t *parent) {
    // Create meter container - full width and height of parent for 3-column layout
    LV_LOCK();
    lv_obj_t *meter_area = lv_obj_create(parent);
    lv_obj_set_size(meter_area, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(meter_area,
                              lv_color_make(0x1A, 0x1A, 0x1A),
                              LV_PART_MAIN);  // Dark gray background to match columns
    lv_obj_set_style_border_width(
        meter_area, 0, LV_PART_MAIN);  // No border since parent column has border
    lv_obj_align(meter_area, LV_ALIGN_TOP_LEFT, 0, 0);

    // Meter title
    lv_obj_t *meter_title = lv_label_create(meter_area);
    lv_label_set_text(meter_title, "Audio Meters");
    lv_obj_set_style_text_color(
        meter_title, lv_color_white(), LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(meter_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(meter_title, LV_ALIGN_TOP_MID, 0, 5);

    // Left channel container
    lv_obj_t *left_channel = lv_obj_create(meter_area);
    lv_obj_set_size(left_channel, lv_pct(45), lv_pct(70));
    lv_obj_set_style_bg_color(
        left_channel, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN);  // Dark gray background
    lv_obj_set_style_border_width(left_channel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(left_channel, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_MAIN);
    lv_obj_align(left_channel, LV_ALIGN_LEFT_MID, 0, 0);

    // Left channel label
    lv_obj_t *left_label = lv_label_create(left_channel);
    lv_label_set_text(left_label, "LEFT");
    lv_obj_set_style_text_color(
        left_label, lv_color_white(), LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(left_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(left_label, LV_ALIGN_TOP_MID, 0, 5);

    // Left channel RMS bar
    m_context.meter_display.bar_l = lv_bar_create(left_channel);
    lv_obj_set_size(m_context.meter_display.bar_l, lv_pct(80), 20);
    lv_obj_align(m_context.meter_display.bar_l, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(m_context.meter_display.bar_l, 0, 100);
    lv_bar_set_value(m_context.meter_display.bar_l, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        m_context.meter_display.bar_l, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_INDICATOR);

    // Left channel peak line (initially hidden) - as child of meter bar
    m_context.meter_display.peak_line_l = lv_obj_create(m_context.meter_display.bar_l);
    lv_obj_set_size(m_context.meter_display.peak_line_l, 2, 20);
    lv_obj_set_style_bg_color(
        m_context.meter_display.peak_line_l, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(m_context.meter_display.peak_line_l, 0, LV_PART_MAIN);
    lv_obj_set_pos(m_context.meter_display.peak_line_l, 0, 0);
    lv_obj_add_flag(m_context.meter_display.peak_line_l, LV_OBJ_FLAG_HIDDEN);

    // Left channel values label
    m_context.meter_display.label_l = lv_label_create(left_channel);
    lv_label_set_text(m_context.meter_display.label_l, "L: 0.000\nPeak: 0.000");
    lv_obj_set_style_text_color(m_context.meter_display.label_l,
                                lv_color_white(),
                                LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(
        m_context.meter_display.label_l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(m_context.meter_display.label_l, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Right channel container
    lv_obj_t *right_channel = lv_obj_create(meter_area);
    lv_obj_set_size(right_channel, lv_pct(45), lv_pct(70));
    lv_obj_set_style_bg_color(
        right_channel, lv_color_make(0x2A, 0x2A, 0x2A), LV_PART_MAIN);  // Dark gray background
    lv_obj_set_style_border_width(right_channel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(right_channel, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_MAIN);
    lv_obj_align(right_channel, LV_ALIGN_RIGHT_MID, 0, 0);

    // Right channel label
    lv_obj_t *right_label = lv_label_create(right_channel);
    lv_label_set_text(right_label, "RIGHT");
    lv_obj_set_style_text_color(
        right_label, lv_color_white(), LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(right_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(right_label, LV_ALIGN_TOP_MID, 0, 5);

    // Right channel RMS bar
    m_context.meter_display.bar_r = lv_bar_create(right_channel);
    lv_obj_set_size(m_context.meter_display.bar_r, lv_pct(80), 20);
    lv_obj_align(m_context.meter_display.bar_r, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(m_context.meter_display.bar_r, 0, 100);
    lv_bar_set_value(m_context.meter_display.bar_r, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        m_context.meter_display.bar_r, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_INDICATOR);

    // Right channel peak line (initially hidden) - as child of meter bar
    m_context.meter_display.peak_line_r = lv_obj_create(m_context.meter_display.bar_r);
    lv_obj_set_size(m_context.meter_display.peak_line_r, 2, 20);
    lv_obj_set_style_bg_color(
        m_context.meter_display.peak_line_r, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(m_context.meter_display.peak_line_r, 0, LV_PART_MAIN);
    lv_obj_set_pos(m_context.meter_display.peak_line_r, 0, 0);
    lv_obj_add_flag(m_context.meter_display.peak_line_r, LV_OBJ_FLAG_HIDDEN);

    // Right channel values label
    m_context.meter_display.label_r = lv_label_create(right_channel);
    lv_label_set_text(m_context.meter_display.label_r, "R: 0.000\nPeak: 0.000");
    lv_obj_set_style_text_color(m_context.meter_display.label_r,
                                lv_color_white(),
                                LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(
        m_context.meter_display.label_r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(m_context.meter_display.label_r, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Peak hold info label
    lv_obj_t *peak_info = lv_label_create(meter_area);
    lv_label_set_text(peak_info, "Peak Hold: 500ms");
    lv_obj_set_style_text_color(
        peak_info, lv_color_white(), LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(peak_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(peak_info, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Populate deferred meter values from current backend snapshot so UI can show immediately
    wavex_meter_data_t md;
    m_context.comm_interface->getMeterData(&md);
    if (md.valid) {
        m_context.deferred_rms_left = md.rms_left;
        m_context.deferred_rms_right = md.rms_right;
        m_context.deferred_peak_left = md.peak_left;
        m_context.deferred_peak_right = md.peak_right;
    }

    // Mark an initial update so the meters render as soon as the page is created
    m_context.meter_update_pending = true;
    markContentChanged();
    ESP_LOGI(TAG,
             "METER_UI: meter widgets created and initial update scheduled L=%.3f R=%.3f",
             m_context.deferred_rms_left,
             m_context.deferred_rms_right);

    // Create LVGL timer to ensure deferred updates are applied in LVGL context
    if (!m_context.meter_lvgl_timer) {
        m_context.meter_lvgl_timer = lv_timer_create(lvglMeterApplyCallback, 50, &m_context);
        if (!m_context.meter_lvgl_timer) {
            ESP_LOGE(TAG, "Failed to create meter LVGL timer");
        }
    }

    LV_UNLOCK();
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

void wavex_ui_create_meter_display(lv_obj_t *parent) {
    if (g_ui_task_instance) {
        g_ui_task_instance->createMeterDisplay(parent);
    }
}
