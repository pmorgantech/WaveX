/**
 * @file ui_task.h
 * @brief UI Task Class Definition
 *
 * This file defines the UITask class that encapsulates all UI-related state
 * and operations. The class provides a clean abstraction for UI management,
 * including LVGL display handling, audio meter display, input processing,
 * and communication with other system components through interfaces.
 *
 * The UITask replaces global state variables with proper encapsulation,
 * improving maintainability and reducing coupling between components.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "comm/i_comm_interface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Forward declarations - only define when not in test mode
#ifndef WAVEX_TEST_BUILD
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_timer_t lv_timer_t;

// LCD panel handle type
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
#endif

// Peak hold data structure
struct PeakHoldData {
    float peak_value = 0.0f;
    uint32_t peak_time_ms = 0;
    bool is_holding = false;
};

#ifndef WAVEX_TEST_BUILD

// Meter display state structure
struct MeterDisplay {
    lv_obj_t *bar = nullptr;
    lv_obj_t *bar_l = nullptr;
    lv_obj_t *bar_r = nullptr;
    lv_obj_t *peak_line_l = nullptr;
    lv_obj_t *peak_line_r = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *label_l = nullptr;
    lv_obj_t *label_r = nullptr;

    // Peak hold tracking
    PeakHoldData peak_hold_l;
    PeakHoldData peak_hold_r;
};

// UI Context - encapsulates all UI state (moved from global to class)
struct UiContext {
    // Task handle
    TaskHandle_t ui_task_handle = NULL;

    // Communication interface
    WaveX::Comm::ICommInterface *comm_interface = nullptr;

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
    volatile float current_rms_left = 0.0f;
    volatile float current_rms_right = 0.0f;
    volatile float current_peak_left = 0.0f;
    volatile float current_peak_right = 0.0f;
    volatile bool meter_callback_data_valid = false;
    volatile uint32_t last_callback_time_ms = 0;
};

// UI Task class - encapsulates UI task state and operations
class UITask {
   public:
    UITask();
    ~UITask() = default;

    // Initialize the UI task
    esp_err_t init();

    // Start the UI task
    esp_err_t start();

    // Stop the UI task
    esp_err_t stop();

    // Mark content as changed (for refresh triggering)
    void markContentChanged();

    // Get display panel handle
    esp_err_t getPanelHandle(esp_lcd_panel_handle_t *panel_handle);

    // Create meter display
    void createMeterDisplay(lv_obj_t *parent);

   private:
    // UI context (encapsulated state)
    UiContext m_context;

    // Private methods
    static void uiTaskFunction(void *pvParameters);
    void run();

    // Meter handling
    static void meterUpdateCallback(void *arg);
    static void lvglMeterApplyCallback(lv_timer_t *timer);
    static void meterDataCallback(
        float rms_left, float rms_right, float peak_left, float peak_right, void *user_data);

    // Peak hold management
    static void updatePeakHoldL(PeakHoldData &peak_data,
                                float current_peak,
                                uint32_t current_time_ms);
    static void updatePeakHoldR(PeakHoldData &peak_data,
                                float current_peak,
                                uint32_t current_time_ms);
    static void updatePeakLinePosition(lv_obj_t *peak_line, lv_obj_t *meter_bar, float peak_value);

    // Adaptive refresh control
    void adaptiveRefreshControl();
};

#endif  // WAVEX_TEST_BUILD

#ifndef WAVEX_TEST_BUILD

// Global functions (for C compatibility)
esp_err_t wavex_ui_task_start(void);
esp_err_t wavex_ui_task_stop(void);
esp_err_t wavex_ui_get_panel_handle(esp_lcd_panel_handle_t *panel_handle);
void wavex_ui_mark_content_changed(void);
void wavex_ui_create_meter_display(lv_obj_t *parent);

#endif  // WAVEX_TEST_BUILD
