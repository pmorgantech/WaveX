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
    // Tabs, in the order the tab bar shows them.
    enum Tab : uint8_t { TAB_SYSTEM = 0, TAB_AUDIO, TAB_LINK, TAB_STORAGE, TAB_MIDI, TAB_COUNT };

    // A metric card: title / value / optional gauge, per docs/ui-design-constraints.md
    // and the WaveX Wireframes v2 card anatomy (305x226, gauge 273x14).
    struct Card {
        lv_obj_t* value;  // the number, font 36
        lv_obj_t* unit;   // suffix, font 22
        lv_obj_t* sub;    // context line, font 18
        lv_obj_t* bar;    // gauge, or nullptr when the metric has no budget
        int warn_pct;     // fill turns orange at/above this; 0 = never
    };

    // UI creation
    void buildTabs(lv_obj_t* parent);
    void buildSystemTab(lv_obj_t* tab);
    void buildLinkTab(lv_obj_t* tab);
    void buildPendingTab(lv_obj_t* tab, const char* what);
    Card makeCard(lv_obj_t* parent,
                  int x,
                  int y,
                  int w,
                  const char* title,
                  const char* tag,
                  bool gauge,
                  int warn_pct);
    void setCard(Card& c, const char* value, const char* unit, const char* sub, int pct);

    // Per-tab refresh
    void refreshSystemTab();
    void refreshLinkTab();

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
    lv_obj_t* tabview;
    uint8_t active_tab;
    bool frozen;  // Freeze softkey: stop refreshing so a transient can be read

    Card sys_cards[8];
    Card link_cards[4];
    lv_obj_t* msg_table;  // per-message-type counts, from wavex_packet_stats_t

    // Set by the sampling timer, consumed by the UI task.
    volatile bool ui_update_pending;
};

/**
 * @brief Factory function for diagnostics page
 */
std::shared_ptr<UIPage> createDiagnosticsPage();

}  // namespace wavex_ui
