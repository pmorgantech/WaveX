// WaveX UI Diagnostics Page
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_page.h"

#include <array>
#include <memory>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

namespace wavex_ui {

/**
 * @brief Diagnostics page for system monitoring
 *
 * Displays real-time ESP32 and Daisy system information including:
 * - CPU usage (cores 0 and 1)
 * - Memory usage (heap)
 * - Uptime
 * - Daisy communication link status
 * - Packet statistics
 * - Audio meter display
 */
class UIDiagnosticsPage : public UIPage {
   public:
    UIDiagnosticsPage();
    ~UIDiagnosticsPage() override;

    // UIPage interface
    const char* name() const override;
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;

   private:
    // UI creation methods
    void createEsp32StatusColumn(lv_obj_t* parent);
    void createDaisyStatusColumn(lv_obj_t* parent);
    void createMetersColumn(lv_obj_t* parent);

    // Diagnostics monitoring
    void startDiagnosticsMonitoring();
    void stopDiagnosticsMonitoring();

    // CPU monitoring methods
    void updateCpuUsage();
    void updateCpuUsageFreertosStats();
    void updateCpuUsageEspIdfBuiltin();

    // Data collection and UI update methods
    void collectDiagnosticsData();
    void applyUiUpdates();

    // Static callbacks
    static void diagnosticsUpdateCallback(void* arg);
    static void lvglUpdateTimerCallback(lv_timer_t* timer);

    // CPU monitoring state
    float cpu_usage_percent;
    float cpu_usage_core0;
    float cpu_usage_core1;
    uint32_t cpu_measurement_count;
    float cpu_usage_history[10];

    // FreeRTOS CPU monitoring
    uint32_t last_total_runtime;
    uint32_t last_idle_runtime_core0;
    uint32_t last_idle_runtime_core1;
    uint32_t last_system_ticks;
    uint32_t last_check_time_ms;

    // ESP-IDF CPU monitoring
    uint32_t last_esp_idf_check_time;

    // Timer handles
    esp_timer_handle_t diagnostics_timer_handle;
    lv_timer_t* lvgl_update_timer;

    // UI elements
    lv_obj_t* diagnostics_label;
    lv_obj_t* daisy_label;

    // Deferred update state
    volatile bool ui_update_pending;
    char deferred_esp32_text[512];
    char deferred_daisy_text[512];
};

/**
 * @brief Factory function for diagnostics page
 */
std::shared_ptr<UIPage> createDiagnosticsPage();

}  // namespace wavex_ui
