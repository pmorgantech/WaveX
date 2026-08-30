/**
 * @file ui_task.h
 * @brief UI Task Class Definition
 *
 * This file defines the UITask class that encapsulates all UI-related state
 * and operations. The class provides a clean abstraction for UI management,
 * including LVGL display handling, input processing, and communication with
 * other system components through interfaces.
 *
 * The audio meters are NOT here: they live in the header status strip
 * (ui_status_strip.cpp), which drives itself from an lv_timer and pulls
 * from inter_mcu_get_meter_data(). This class used to carry a parallel,
 * unreachable meter pipeline as well - see CHANGELOG 2026-08-29.
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

#include <atomic>

// Forward declarations - only define when not in test mode
#ifndef WAVEX_TEST_BUILD
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_timer_t lv_timer_t;

// LCD panel handle type
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;
#endif

#ifndef WAVEX_TEST_BUILD

// UI Context - encapsulates all UI state (moved from global to class)
struct UiContext {
    // Task handle. Written by start()/run() (different tasks/cores) and
    // polled by stop() from whichever task calls it - atomic per
    // docs/esp32p4_coding_guide.md (do not use a plain pointer or `volatile`
    // for cross-task synchronization).
    std::atomic<TaskHandle_t> ui_task_handle{NULL};

    // Communication interface
    WaveX::Comm::ICommInterface *comm_interface = nullptr;

    // Adaptive refresh rate control
    bool content_changed = false;
    uint32_t last_refresh_time = 0;
    uint32_t refresh_count = 0;

    // Encoder delta accumulation for detent-based events
    int32_t pcnt1_delta_accumulator = 0;
};

// UI Task class - encapsulates UI task state and operations
class UITask {
   public:
    explicit UITask(WaveX::Comm::ICommInterface &comm_interface);
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

   private:
    // Injected dependencies
    WaveX::Comm::ICommInterface &m_comm_interface;

    // UI context (encapsulated state)
    UiContext m_context;

    // Private methods
    static void uiTaskFunction(void *pvParameters);
    void run();

    // Adaptive refresh control
    void adaptiveRefreshControl();
};

#endif  // WAVEX_TEST_BUILD

#ifndef WAVEX_TEST_BUILD

// Global functions (for C compatibility)
esp_err_t wavex_ui_task_start(WaveX::Comm::ICommInterface &comm_interface);
esp_err_t wavex_ui_task_stop(void);
esp_err_t wavex_ui_get_panel_handle(esp_lcd_panel_handle_t *panel_handle);
void wavex_ui_mark_content_changed(void);

#endif  // WAVEX_TEST_BUILD
