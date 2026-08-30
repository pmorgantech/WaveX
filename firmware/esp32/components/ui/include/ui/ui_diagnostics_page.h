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
        lv_obj_t* bar2;   // second gauge (dual-core tile), or nullptr
        lv_obj_t* spark;  // lv_chart sparkline, or nullptr
        lv_chart_series_t* series;
        int warn_pct;  // fill turns orange at/above this; 0 = never
    };

    // Sparkline width. The sampling timer runs at 500 ms, so 60 points is
    // 30 s of history - long enough to show a trend, which is the whole reason
    // a sparkline beats a number: "CPU is 40%" versus "CPU has been climbing
    // for half a minute" is the difference between noticing and diagnosing.
    static constexpr uint16_t kSparkPoints = 60;

    // UI creation
    void buildTabs(lv_obj_t* parent);
    void buildSystemTab(lv_obj_t* tab);
    void buildLinkTab(lv_obj_t* tab);
    void buildAudioTab(lv_obj_t* tab);
    void buildStorageTab(lv_obj_t* tab);
    void buildMidiTab(lv_obj_t* tab);
    Card makeCard(lv_obj_t* parent,
                  int x,
                  int y,
                  int w,
                  const char* title,
                  const char* tag,
                  bool gauge,
                  int warn_pct);
    void setCard(Card& c, const char* value, const char* unit, const char* sub, int pct);
    // Add a sparkline and, optionally, a second gauge bar to an existing card.
    void addSpark(Card& c, uint32_t colour);
    void addSecondBar(Card& c, uint32_t colour);
    void pushSpark(Card& c, int value);

    // Per-tab refresh
    void refreshSystemTab();
    void refreshLinkTab();
    void refreshAudioTab();
    void refreshStorageTab();
    void refreshMidiTab();
    // Shared "backend has gone quiet" rendering, so a stale figure is never
    // presented as a current one.
    void showTabOffline(Card* cards, int n, const char* why);

    // Diagnostics monitoring
    void startDiagnosticsMonitoring();
    void stopDiagnosticsMonitoring();

    // CPU monitoring methods
    void updateCpuUsage();
    void updateCpuUsageFreertosStats();

    // Data collection and UI update methods
    void collectDiagnosticsData();
    void applyUiUpdates();

    // Single path for changing tabs, whether by touch or by softkey. Touch
    // goes through the tabview's own event, so both must land here or the
    // refresh loop and the visible tab drift apart.
    static void onTabChanged(lv_event_t* e);
    void setActiveTab(uint8_t tab);

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
    uint32_t last_check_time_ms;

    // ESP-IDF CPU monitoring

    // Timer handles
    esp_timer_handle_t diagnostics_timer_handle;
    lv_timer_t* lvgl_update_timer;

    // UI elements
    lv_obj_t* tabview;
    uint8_t active_tab;
    bool frozen;  // Freeze softkey: stop refreshing so a transient can be read

    Card sys_cards[8];
    Card link_cards[4];
    Card audio_cards[8];
    Card storage_cards[8];
    Card midi_cards[4];
    lv_obj_t* midi_note;  // "no sequencer yet" explainer on the MIDI tab
    lv_obj_t* msg_table;  // per-message-type counts, from wavex_packet_stats_t

    // Set by the sampling timer, consumed by the UI task.
    volatile bool ui_update_pending;
};

/**
 * @brief Factory function for diagnostics page
 */
std::shared_ptr<UIPage> createDiagnosticsPage();

}  // namespace wavex_ui
