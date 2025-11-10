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

// Peak hold tracking
struct PeakHoldData {
    float peak_value;
    uint32_t peak_time_ms;
    bool is_holding;
};
static const uint32_t PEAK_HOLD_TIMEOUT_MS = 500;  // Configurable peak hold duration

// Meter display state (grouped for better organization and scalability)
struct MeterDisplay {
    // Legacy single meter (fallback)
    lv_obj_t *bar = nullptr;
    lv_obj_t *label = nullptr;

    // Enhanced dual-channel meters
    lv_obj_t *bar_l = nullptr;
    lv_obj_t *bar_r = nullptr;
    lv_obj_t *peak_line_l = nullptr;
    lv_obj_t *peak_line_r = nullptr;
    lv_obj_t *label_l = nullptr;
    lv_obj_t *label_r = nullptr;

    // Peak hold tracking
    PeakHoldData peak_hold_l = {0.0f, 0, false};
    PeakHoldData peak_hold_r = {0.0f, 0, false};
};

// UI Context - encapsulates all UI state to reduce global variables
struct UiContext {
    // Task handle
    TaskHandle_t ui_task_handle = NULL;

    // Display and LVGL handles
    esp_timer_handle_t meter_timer_handle = NULL;
    lv_timer_t *meter_lvgl_timer = NULL;

    // Meter display state
    MeterDisplay meter_display;

    // Adaptive refresh rate control
    bool content_changed = false;
    uint32_t last_refresh_time = 0;
    uint32_t refresh_count = 0;

    // Deferred meter update data (shared between timer and UI task)
    volatile bool meter_update_pending = false;
    volatile bool meter_reset_pending = false;
    volatile float deferred_rms_left = 0.0f;
    volatile float deferred_rms_right = 0.0f;
    volatile float deferred_peak_left = 0.0f;
    volatile float deferred_peak_right = 0.0f;

    // Current meter values (from Daisy callback)
    volatile float current_rms = 0.0f;
    volatile float current_peak = 0.0f;
};

// Global UI context instance (single instance for this module)
static UiContext g_ui_context;

// Adaptive refresh rate control constants
static const uint32_t MIN_REFRESH_INTERVAL_MS = 16;   // 60 FPS maximum
static const uint32_t MAX_REFRESH_INTERVAL_MS = 100;  // 10 FPS minimum

// Forward declarations
static void ui_task(void *pvParameters);
static void meter_update_cb(void *arg);
static void lvgl_meter_apply_cb(lv_timer_t *timer);
static void meter_data_cb(float rms, float peak, void *user_data);
static void adaptive_refresh_control(void);
void wavex_ui_mark_content_changed(void);

// Enhanced meter functions
static void update_peak_hold(PeakHoldData *peak_data, float current_peak, uint32_t current_time_ms);
static void update_peak_line_position(lv_obj_t *peak_line, lv_obj_t *meter_bar, float peak_value);
void wavex_ui_create_meter_display(lv_obj_t *parent);

/**
 * @brief Update peak hold tracking for a channel
 */
static void update_peak_hold(PeakHoldData *peak_data,
                             float current_peak,
                             uint32_t current_time_ms) {
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
static void update_peak_line_position(lv_obj_t *peak_line, lv_obj_t *meter_bar, float peak_value) {
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

/**
 * @brief Meter update timer callback for real-time updates
 */
static void meter_update_cb(void *arg) {
    UiContext *ctx = (UiContext *)arg;
    if (!ctx)
        return;
    // Get current meter data from statistics
    wavex_meter_data_t meter_data;
    inter_mcu_get_meter_data(&meter_data);
    // Instrumentation: log received meter snapshot for debugging UI updates
    // ESP_LOGI(TAG, "METER_CB: valid=%d last_update_ms=%u rms_l=%.3f rms_r=%.3f peak_l=%.3f
    // peak_r=%.3f",
    //          meter_data.valid ? 1 : 0,
    //          meter_data.last_update_ms,
    //          meter_data.rms_left, meter_data.rms_right,
    //          meter_data.peak_left, meter_data.peak_right);

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

        if (time_since_reset > 1000) {  // Only reset once per second
            // Reset peak hold values to 0
            ctx->meter_display.peak_hold_l.peak_value = 0.0f;
            ctx->meter_display.peak_hold_l.peak_time_ms = current_time_ms;
            ctx->meter_display.peak_hold_l.is_holding = false;

            ctx->meter_display.peak_hold_r.peak_value = 0.0f;
            ctx->meter_display.peak_hold_r.peak_time_ms = current_time_ms;
            ctx->meter_display.peak_hold_r.is_holding = false;

            last_reset_time = current_time_ms;

            // ESP_LOGI(TAG, "METER_CB: resetting meters (time_since_reset=%u)",
            // (unsigned)time_since_reset); Mark meter reset as pending (will be processed by main
            // UI task)
            ctx->meter_reset_pending = true;
            wavex_ui_mark_content_changed();
        }
    } else if (meter_data.valid) {
        // Normal operation - update meters with valid data
        // Update peak hold tracking
        update_peak_hold(&ctx->meter_display.peak_hold_l, meter_data.peak_left, current_time_ms);
        update_peak_hold(&ctx->meter_display.peak_hold_r, meter_data.peak_right, current_time_ms);

        // Store meter data for deferred update (no LVGL locks in timer callback)
        ctx->deferred_rms_left = meter_data.rms_left;
        ctx->deferred_rms_right = meter_data.rms_right;
        ctx->deferred_peak_left = ctx->meter_display.peak_hold_l.peak_value;
        ctx->deferred_peak_right = ctx->meter_display.peak_hold_r.peak_value;

        // ESP_LOGI(TAG, "METER_CB: scheduling meter update rms_l=%.3f rms_r=%.3f peak_l=%.3f
        // peak_r=%.3f",
        //          ctx->deferred_rms_left, ctx->deferred_rms_right, ctx->deferred_peak_left,
        //          ctx->deferred_peak_right);
        // Mark meter update as pending (will be processed by main UI task)
        ctx->meter_update_pending = true;
        wavex_ui_mark_content_changed();
    }
}

// Removed: process_deferred_meter_updates() - now handled exclusively by LVGL timer
// (lvgl_meter_apply_cb)

/**
 * @brief LVGL timer callback to apply deferred meter updates in LVGL context.
 * Runs with LVGL lock already held.
 */
static void lvgl_meter_apply_cb(lv_timer_t *timer) {
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
            update_peak_line_position(
                ctx->meter_display.peak_line_l, ctx->meter_display.bar_l, 0.0f);
            update_peak_line_position(
                ctx->meter_display.peak_line_r, ctx->meter_display.bar_r, 0.0f);
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
            update_peak_line_position(
                ctx->meter_display.peak_line_l, ctx->meter_display.bar_l, ctx->deferred_peak_left);
            update_peak_line_position(
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

/**
 * @brief Create enhanced dual-channel meter display with peak hold
 */
void wavex_ui_create_meter_display(lv_obj_t *parent) {
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
    g_ui_context.meter_display.bar_l = lv_bar_create(left_channel);
    lv_obj_set_size(g_ui_context.meter_display.bar_l, lv_pct(80), 20);
    lv_obj_align(g_ui_context.meter_display.bar_l, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(g_ui_context.meter_display.bar_l, 0, 100);
    lv_bar_set_value(g_ui_context.meter_display.bar_l, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        g_ui_context.meter_display.bar_l, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_INDICATOR);

    // Left channel peak line (initially hidden) - as child of meter bar
    g_ui_context.meter_display.peak_line_l = lv_obj_create(g_ui_context.meter_display.bar_l);
    lv_obj_set_size(g_ui_context.meter_display.peak_line_l, 2, 20);
    lv_obj_set_style_bg_color(
        g_ui_context.meter_display.peak_line_l, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_ui_context.meter_display.peak_line_l, 0, LV_PART_MAIN);
    lv_obj_set_pos(g_ui_context.meter_display.peak_line_l, 0, 0);
    lv_obj_add_flag(g_ui_context.meter_display.peak_line_l, LV_OBJ_FLAG_HIDDEN);

    // Left channel values label
    g_ui_context.meter_display.label_l = lv_label_create(left_channel);
    lv_label_set_text(g_ui_context.meter_display.label_l, "L: 0.000\nPeak: 0.000");
    lv_obj_set_style_text_color(g_ui_context.meter_display.label_l,
                                lv_color_white(),
                                LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(
        g_ui_context.meter_display.label_l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_ui_context.meter_display.label_l, LV_ALIGN_BOTTOM_MID, 0, -5);

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
    g_ui_context.meter_display.bar_r = lv_bar_create(right_channel);
    lv_obj_set_size(g_ui_context.meter_display.bar_r, lv_pct(80), 20);
    lv_obj_align(g_ui_context.meter_display.bar_r, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(g_ui_context.meter_display.bar_r, 0, 100);
    lv_bar_set_value(g_ui_context.meter_display.bar_r, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        g_ui_context.meter_display.bar_r, lv_color_make(0x4C, 0xAF, 0x50), LV_PART_INDICATOR);

    // Right channel peak line (initially hidden) - as child of meter bar
    g_ui_context.meter_display.peak_line_r = lv_obj_create(g_ui_context.meter_display.bar_r);
    lv_obj_set_size(g_ui_context.meter_display.peak_line_r, 2, 20);
    lv_obj_set_style_bg_color(
        g_ui_context.meter_display.peak_line_r, lv_color_make(0xFF, 0x57, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(g_ui_context.meter_display.peak_line_r, 0, LV_PART_MAIN);
    lv_obj_set_pos(g_ui_context.meter_display.peak_line_r, 0, 0);
    lv_obj_add_flag(g_ui_context.meter_display.peak_line_r, LV_OBJ_FLAG_HIDDEN);

    // Right channel values label
    g_ui_context.meter_display.label_r = lv_label_create(right_channel);
    lv_label_set_text(g_ui_context.meter_display.label_r, "R: 0.000\nPeak: 0.000");
    lv_obj_set_style_text_color(g_ui_context.meter_display.label_r,
                                lv_color_white(),
                                LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(
        g_ui_context.meter_display.label_r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(g_ui_context.meter_display.label_r, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Peak hold info label
    lv_obj_t *peak_info = lv_label_create(meter_area);
    lv_label_set_text(peak_info, "Peak Hold: 500ms");
    lv_obj_set_style_text_color(
        peak_info, lv_color_white(), LV_PART_MAIN);  // White text for dark mode
    lv_obj_set_style_text_font(peak_info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(peak_info, LV_ALIGN_BOTTOM_MID, 0, -5);

    // Populate deferred meter values from current backend snapshot so UI can show immediately
    wavex_meter_data_t md;
    inter_mcu_get_meter_data(&md);
    if (md.valid) {
        g_ui_context.deferred_rms_left = md.rms_left;
        g_ui_context.deferred_rms_right = md.rms_right;
        g_ui_context.deferred_peak_left = md.peak_left;
        g_ui_context.deferred_peak_right = md.peak_right;
    }

    // Mark an initial update so the meters render as soon as the page is created
    g_ui_context.meter_update_pending = true;
    wavex_ui_mark_content_changed();
    ESP_LOGI(TAG,
             "METER_UI: meter widgets created and initial update scheduled L=%.3f R=%.3f",
             g_ui_context.deferred_rms_left,
             g_ui_context.deferred_rms_right);
    // Create LVGL timer to ensure deferred updates are applied in LVGL context
    if (!g_ui_context.meter_lvgl_timer) {
        g_ui_context.meter_lvgl_timer = lv_timer_create(lvgl_meter_apply_cb, 50, &g_ui_context);
        if (!g_ui_context.meter_lvgl_timer) {
            ESP_LOGE(TAG, "Failed to create meter LVGL timer");
        }
    }
}

/**
 * @brief Meter data callback from Daisy
 * Stores real-time meter data in UI context for potential future use
 */
static void meter_data_cb(float rms, float peak, void *user_data) {
    UiContext *ctx = (UiContext *)user_data;
    if (!ctx)
        return;

// Debug logging for meter data
#ifdef WAVEX_LOG_METER_DATA
    ESP_LOGI(TAG, "Meter data received: RMS=%.3f, Peak=%.3f", rms, peak);
#endif

    // Store real-time meter data in context
    ctx->current_rms = rms;
    ctx->current_peak = peak;
}

/**
 * @brief Mark content as changed to trigger refresh
 */
void wavex_ui_mark_content_changed(void) {
    g_ui_context.content_changed = true;
}

/**
 * @brief Adaptive refresh rate control for optimal performance
 */
static void adaptive_refresh_control(UiContext *ctx) {
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);  // Convert to ms

    // Only refresh if content has changed and enough time has passed
    if (ctx->content_changed) {
        uint32_t time_since_last_refresh = current_time - ctx->last_refresh_time;

        // Use minimum refresh interval for responsive updates
        if (time_since_last_refresh >= MIN_REFRESH_INTERVAL_MS) {
            if (auto *display = wavex_ui::DisplayManager::instance().display()) {
                LV_LOCK();
                lv_refr_now(display);
                LV_UNLOCK();
            }

            ctx->content_changed = false;
            ctx->last_refresh_time = current_time;
            ctx->refresh_count++;

            // Log refresh rate every 100 refreshes for monitoring
            if (ctx->refresh_count % 100 == 0) {
                ESP_LOGD(TAG, "Display refresh count: %lu", ctx->refresh_count);
            }
        }
    }
}

/**
 * @brief UI task implementation
 */
static void ui_task(void *pvParameters) {
    ESP_LOGI(TAG, "UI task started with full UI support");

    // Initialize LVGL display
    ESP_LOGI(TAG, "Initializing LVGL display...");

    // Initialize LVGL display
    esp_err_t lvgl_ret = wavex_ui::DisplayManager::instance().init();
    if (lvgl_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL display: %s", esp_err_to_name(lvgl_ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    // Log memory status
    ESP_LOGI(TAG, "Memory after display init:");
    ESP_LOGI(TAG, "  Free heap: %zu bytes", esp_get_free_heap_size());

    // Defer layout construction to the navigator stack
    ESP_LOGI(TAG, "Handing layout control to navigator stack");

    // Create meter update timer (every 33ms for 30 FPS real-time updates)
    const esp_timer_create_args_t meter_timer_args = {.callback = &meter_update_cb,
                                                      .arg = &g_ui_context,
                                                      .dispatch_method = ESP_TIMER_TASK,
                                                      .name = "meter_timer",
                                                      .skip_unhandled_events = false};
    esp_err_t timer_ret = esp_timer_create(&meter_timer_args, &g_ui_context.meter_timer_handle);
    if (timer_ret == ESP_OK) {
        esp_timer_start_periodic(g_ui_context.meter_timer_handle, 33000); // 33ms in microseconds (30 FPS)
        ESP_LOGI(TAG, "Meter timer started (33ms real-time updates - 30 FPS)");
    } else {
        ESP_LOGE(TAG, "Failed to create meter timer: %s", esp_err_to_name(timer_ret));
    }

    // Register meter data callback for real-time audio meter updates
    inter_mcu_set_meter_listener(meter_data_cb, &g_ui_context);
    ESP_LOGI(TAG, "Meter data callback registered");

    // Test meter callback with dummy data to verify it's working
    ESP_LOGI(TAG, "Testing meter callback with dummy data...");
    meter_data_cb(0.5f, 0.8f, &g_ui_context);  // Test with 50% RMS, 80% peak

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
        // // If deferred update still pending (LVGL timers or lock contention), ensure it is
        // applied here if (s_meter_update_pending || s_meter_reset_pending) {
        //     LV_LOCK();
        //     ESP_LOGI(TAG, "UI_LOOP: forcing LVGL meter apply (pending=%d reset=%d)",
        //     s_meter_update_pending ? 1 : 0, s_meter_reset_pending ? 1 : 0);
        //     lvgl_meter_apply_cb(NULL);
        //     LV_UNLOCK();
        // }

        // Process deferred sample browser updates (prevents deadlock from SPI/UART task)
        // Acquire LVGL lock before processing deferred updates
        LV_LOCK();
        wavex_ui::UISampleBrowser::processDeferredUpdates();
        LV_UNLOCK();

        // Use adaptive refresh control for optimal performance
        adaptive_refresh_control(&g_ui_context);

        // Short delay to prevent excessive CPU usage
        vTaskDelay(pdMS_TO_TICKS(32));  // 32ms delay for 30 FPS theoretical maximum
    }
}

esp_err_t wavex_ui_task_start(void) {
    ESP_LOGI(TAG, "Starting UI task with MIPI DSI display and LVGL...");

    // Create UI task on Core 1 (different from SPI which typically runs on Core 0)
    BaseType_t task_ret =
        xTaskCreatePinnedToCore(ui_task,
                                "ui_task",
                                16384,  // Increased stack size to 16KB for LVGL and diagnostics
                                NULL,
                                2,  // Priority
                                &g_ui_context.ui_task_handle,
                                1  // Run on Core 1
        );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UI task started successfully");
    return ESP_OK;
}

esp_err_t wavex_ui_task_stop(void) {
    ESP_LOGI(TAG, "Stopping UI task...");

    if (g_ui_context.ui_task_handle != NULL) {
        vTaskDelete(g_ui_context.ui_task_handle);
        g_ui_context.ui_task_handle = NULL;
    }

    // Stop diagnostics page module
    diagnostics_page_stop();

    // Stop and delete the meter timer
    if (g_ui_context.meter_timer_handle) {
        esp_timer_stop(g_ui_context.meter_timer_handle);
        esp_timer_delete(g_ui_context.meter_timer_handle);
        g_ui_context.meter_timer_handle = NULL;
    }

    // Delete LVGL timer
    if (g_ui_context.meter_lvgl_timer) {
        lv_timer_delete(g_ui_context.meter_lvgl_timer);
        g_ui_context.meter_lvgl_timer = NULL;
    }

    // Let display manager clean up LVGL and touch resources
    wavex_ui::DisplayManager::instance().deinit();

    ESP_LOGI(TAG, "UI task stopped");
    return ESP_OK;
}

esp_err_t wavex_ui_get_panel_handle(esp_lcd_panel_handle_t *panel_handle) {
    return wavex_ui::DisplayManager::instance().panelHandle(panel_handle);
}
