/**
 * @file wavex_application.cpp
 * @brief WaveX Application Implementation
 */

#include "wavex_application.h"

#include <inttypes.h>
#include <stdio.h>

#include "config/link_config.h"
#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <stdio.h>
#define ESP_LOGI(TAG, FMT, ...) ((void)0)
#define ESP_LOGE(TAG, FMT, ...) ((void)0)
static inline unsigned int esp_get_free_heap_size() {
    return 0U;
}
static inline long long esp_timer_get_time() {
    return 0;
}
typedef int esp_err_t;
#define ESP_OK 0
#endif

// Include WaveX configuration
#include "config.h"

// esp_err.h is brought in transitively by ESP-IDF headers when building on target
#include "inter_mcu.h"
#include "ui_task.h"
#include "version.h"

// Bring in the SPI link API
#if WAVEX_SPI_LINK_ENABLED
#include "links/esp_spi_link.h"
#endif

// Bring in the PCNT task API
#ifndef WAVEX_TEST_BUILD
#include "pcnt_task.h"
// MIDI input tasks (roadmap Phase 1 item 8)
#include "midi_task.h"
#include "usb_midi_task.h"
#endif

static const char *TAG = "WaveXApplication";

#ifdef WAVEX_TEST_BUILD
// Mock function declarations for test builds are in the test files
#endif

namespace WaveX {

WaveXApplication::WaveXApplication() : m_initialized(false), m_loopCounter(0), m_lastHeapLogTime(0) {
    ESP_LOGI(TAG, "WaveX Application created");
}

bool WaveXApplication::initialize() {
    if (m_initialized) {
        ESP_LOGW(TAG, "Application already initialized");
        return true;
    }

    ESP_LOGI(TAG, "=== WaveX ESP32 Frontend Starting ===");
    ESP_LOGI(TAG, "Version: %s", WAVEX_FRONTEND_VERSION_STRING);
    ESP_LOGI(TAG, "Built: %s %s", WAVEX_COMPILE_DATE, WAVEX_COMPILE_TIME);
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    // Initialize subsystems in order
    if (!initializeInterMCU()) {
        ESP_LOGE(TAG, "Failed to initialize inter-MCU communication");
        return false;
    }

    if (!initializePCNT()) {
        ESP_LOGE(TAG, "Failed to initialize PCNT");
        return false;
    }

    // MIDI input (Phase 1 item 8) - after inter-MCU so forwarded notes
    // have a live link. Not fatal on failure: the instrument works without
    // MIDI, so log and continue.
#ifndef WAVEX_TEST_BUILD
    if (midi_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "DIN MIDI init failed - continuing without DIN MIDI input");
    }
    if (usb_midi_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "USB MIDI init failed - continuing without USB MIDI input");
    }
#endif

    // Brief delay before UI initialization to avoid contention
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(200));
#endif

    if (!initializeUI()) {
        ESP_LOGE(TAG, "Failed to initialize UI");
        return false;
    }

    ESP_LOGI(TAG, "WaveX ESP32 Frontend Initialized");
    ESP_LOGI(TAG, "About menu available under System -> About");

    m_initialized = true;
    return true;
}

void WaveXApplication::run() {
    if (!m_initialized) {
        ESP_LOGE(TAG, "Cannot run application - not initialized");
        return;
    }

    ESP_LOGI(TAG, "Main loop started");

    while (true) {
        m_loopCounter++;

        // Log system status periodically
        logSystemStatus();

        // UART operations are handled by inter_mcu

#ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(1000));  // 2 second loop - slower to reduce load
#endif
    }
}

bool WaveXApplication::initializeInterMCU() {
    ESP_LOGI(TAG, "Initializing inter-MCU communication");

    esp_err_t result = inter_mcu_init(m_context.getStatistics());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Inter-MCU initialization failed: %s", esp_err_to_name(result));
        return false;
    }

    // Start inter-MCU communication (UART link)
    esp_err_t start_result = inter_mcu_start();
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Inter-MCU start failed: %s", esp_err_to_name(start_result));
        return false;
    }

    ESP_LOGI(TAG, "Inter-MCU communication initialized successfully");
    return true;
}

bool WaveXApplication::initializePCNT() {
    ESP_LOGI(TAG, "Initializing PCNT encoders");

    esp_err_t result = pcnt_task_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "PCNT initialization failed: %s", esp_err_to_name(result));
        return false;
    }

    // Start PCNT reading task
    esp_err_t start_result = pcnt_task_start();
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "PCNT task start failed: %s", esp_err_to_name(start_result));
        return false;
    }

    ESP_LOGI(TAG, "PCNT encoders initialized successfully");
    return true;
}

bool WaveXApplication::initializeUI() {
#if WAVEX_LCD_DISPLAY_ENABLED && (WAVEX_LCD_DISPLAY_TYPE == 1)
    ESP_LOGI(TAG, "Initializing UI system");

    esp_err_t result = wavex_ui_task_start(m_context.getCommInterface());
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "UI initialization failed: %s", esp_err_to_name(result));
        return false;
    }

    ESP_LOGI(TAG, "UI system initialized successfully");
    return true;
#else
    ESP_LOGI(TAG, "MIPI DSI UI disabled for this configuration");
    return true;
#endif
}

void WaveXApplication::logSystemStatus() {
    int current_time = (int)(esp_timer_get_time() / 1000000);

    if (current_time - m_lastHeapLogTime >= 60) { // Log every 60 seconds
        ESP_LOGI(TAG,
                 "System status - Loop: %d, Free heap: %" PRIu32 " bytes",
                 m_loopCounter,
                 esp_get_free_heap_size());
        m_lastHeapLogTime = current_time;
    }
}

}  // namespace WaveX
