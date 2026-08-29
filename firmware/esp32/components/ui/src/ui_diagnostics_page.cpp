// WaveX UI Diagnostics Page Implementation
#include "ui/ui_diagnostics_page.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <string.h>

#include "comm/statistics.h"
#include "config.h"
#include "config/link_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inter_mcu.h"
#include "links/esp_spi_link.h"
#include "ui/ui_navigator.h"
#include "ui/ui_sample_memory_page.h"
#include "ui_task.h"

#include <memory>

static const char* TAG = "UI_DIAGNOSTICS_PAGE";

namespace wavex_ui {

UIDiagnosticsPage::UIDiagnosticsPage()
    : cpu_usage_percent(0.0f),
      cpu_usage_core0(0.0f),
      cpu_usage_core1(0.0f),
      cpu_measurement_count(0),
      last_total_runtime(0),
      last_idle_runtime_core0(0),
      last_idle_runtime_core1(0),
      last_system_ticks(0),
      last_check_time_ms(0),
      last_esp_idf_check_time(0),
      diagnostics_timer_handle(nullptr),
      lvgl_update_timer(nullptr),
      tabview(nullptr),
      active_tab(TAB_SYSTEM),
      frozen(false),
      msg_table(nullptr),
      ui_update_pending(false) {
    memset(sys_cards, 0, sizeof(sys_cards));
    memset(link_cards, 0, sizeof(link_cards));
    memset(cpu_usage_history, 0, sizeof(cpu_usage_history));
}

UIDiagnosticsPage::~UIDiagnosticsPage() {
    // Cleanup is done in onExit()
}

const char* UIDiagnosticsPage::name() const {
    return "Diagnostics";
}

namespace {

// Design palette (WaveX Wireframes v2). ui_theme.h covers the shared subset;
// these are the additional greys the diagnostics cards use.
constexpr uint32_t kColBg = 0x000000;
constexpr uint32_t kColCard = 0x141414;
constexpr uint32_t kColBorder = 0x333333;
constexpr uint32_t kColDim = 0x8FA0AA;
constexpr uint32_t kColDimmer = 0x6E7A82;
constexpr uint32_t kColTrack = 0x262B2E;
constexpr uint32_t kColTabOn = 0x10293B;
constexpr uint32_t kColGreen = 0x4CAF50;
constexpr uint32_t kColOrange = 0xFF5722;
constexpr uint32_t kColBlue = 0x2196F3;

// Card grid, from the design: 4 across, 305x226, gutters 12.
constexpr int kCardW = 305;
constexpr int kCardH = 226;
constexpr int kGaugeW = 273;
constexpr int kColX[4] = {12, 329, 646, 963};
constexpr int kRowY[2] = {0, 238};  // relative to the tab body

lv_obj_t* mkLabel(
    lv_obj_t* parent, int x, int y, const char* txt, const lv_font_t* font, uint32_t colour) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

}  // namespace

void UIDiagnosticsPage::onEnter(lv_obj_t* parent) {
    ESP_LOGI(TAG, "Diagnostics page entering");

    lv_obj_clean(parent);
    msg_table = nullptr;
    frozen = false;
    memset(sys_cards, 0, sizeof(sys_cards));
    memset(link_cards, 0, sizeof(link_cards));

    buildTabs(parent);
    startDiagnosticsMonitoring();
}

void UIDiagnosticsPage::buildTabs(lv_obj_t* parent) {
    tabview = lv_tabview_create(parent);
    lv_tabview_set_tab_bar_size(tabview, 56);
    lv_obj_set_size(tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tabview, lv_color_hex(kColBg), 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColBg), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColDimmer), 0);
    // Selected tab cell: filled, white text, 4px blue underline. Composed once
    // because mixing lv_part_t with lv_state_t warns under C++20.
    const lv_style_selector_t sel_on = static_cast<lv_style_selector_t>(LV_PART_ITEMS) |
                                       static_cast<lv_style_selector_t>(LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColTabOn), sel_on);
    lv_obj_set_style_text_color(bar, lv_color_white(), sel_on);
    lv_obj_set_style_border_color(bar, lv_color_hex(kColBlue), sel_on);
    lv_obj_set_style_border_width(bar, 4, sel_on);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, sel_on);

    lv_obj_t* t_sys = lv_tabview_add_tab(tabview, "System");
    lv_obj_t* t_audio = lv_tabview_add_tab(tabview, "Audio");
    lv_obj_t* t_link = lv_tabview_add_tab(tabview, "Link");
    lv_obj_t* t_storage = lv_tabview_add_tab(tabview, "Storage");
    lv_obj_t* t_midi = lv_tabview_add_tab(tabview, "MIDI");

    lv_obj_t* tabs[TAB_COUNT] = {t_sys, t_audio, t_link, t_storage, t_midi};
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(kColBg), 0);
        lv_obj_set_style_pad_all(tabs[i], 0, 0);
        lv_obj_set_style_border_width(tabs[i], 0, 0);
        lv_obj_remove_flag(tabs[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    buildSystemTab(t_sys);
    buildLinkTab(t_link);
    // Audio / Storage / MIDI are mostly Daisy-side figures that do not cross
    // the link yet. Showing the layout with invented numbers would be worse
    // than saying so - see docs/ui-diagnostics-spec.md (MSG_DIAG_PUSH).
    buildPendingTab(t_audio, "Audio");
    buildPendingTab(t_storage, "Storage");
    buildPendingTab(t_midi, "MIDI");

    lv_tabview_set_active(tabview, active_tab, LV_ANIM_OFF);
}

UIDiagnosticsPage::Card UIDiagnosticsPage::makeCard(lv_obj_t* parent,
                                                    int x,
                                                    int y,
                                                    int w,
                                                    const char* title,
                                                    const char* tag,
                                                    bool gauge,
                                                    int warn_pct) {
    Card c = {};
    c.warn_pct = warn_pct;

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, kCardH);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColCard), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    mkLabel(card, 16, 14, title, &lv_font_montserrat_18, kColDim);
    if (tag) {
        // Source tag ("esp32" / "wire" / "new") - which side owns the number.
        uint32_t tc = kColDimmer;
        if (strcmp(tag, "wire") == 0)
            tc = kColBlue;
        else if (strcmp(tag, "new") == 0)
            tc = kColOrange;
        lv_obj_t* l = mkLabel(card, 0, 0, tag, &lv_font_montserrat_14, tc);
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -16, 14);
    }

    c.value = mkLabel(card, 16, 64, "-", &lv_font_montserrat_36, 0xFFFFFF);
    c.unit = mkLabel(card, 0, 0, "", &lv_font_montserrat_22, kColDim);
    lv_obj_align_to(c.unit, c.value, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -6);

    c.sub = mkLabel(card, 16, 112, "", &lv_font_montserrat_18, kColDim);
    lv_obj_set_width(c.sub, w - 32);
    lv_label_set_long_mode(c.sub, LV_LABEL_LONG_WRAP);

    if (gauge) {
        c.bar = lv_bar_create(card);
        lv_obj_set_size(c.bar, kGaugeW, 14);
        lv_obj_set_pos(c.bar, 16, 196);
        lv_bar_set_range(c.bar, 0, 100);
        lv_bar_set_value(c.bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(c.bar, lv_color_hex(kColTrack), LV_PART_MAIN);
        lv_obj_set_style_bg_color(c.bar, lv_color_hex(kColGreen), LV_PART_INDICATOR);
        lv_obj_set_style_radius(c.bar, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(c.bar, 2, LV_PART_INDICATOR);
    }
    return c;
}

void UIDiagnosticsPage::setCard(
    Card& c, const char* value, const char* unit, const char* sub, int pct) {
    if (!c.value)
        return;
    lv_label_set_text(c.value, value);
    lv_label_set_text(c.unit, unit ? unit : "");
    lv_obj_align_to(c.unit, c.value, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -6);
    lv_label_set_text(c.sub, sub ? sub : "");
    if (c.bar) {
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        lv_bar_set_value(c.bar, pct, LV_ANIM_OFF);
        // Orange is a real warning, not decoration: only where high is bad.
        const bool warn = (c.warn_pct > 0 && pct >= c.warn_pct);
        lv_obj_set_style_bg_color(
            c.bar, lv_color_hex(warn ? kColOrange : kColGreen), LV_PART_INDICATOR);
    }
}

void UIDiagnosticsPage::buildSystemTab(lv_obj_t* tab) {
    // Every figure here is ESP32-local, so this tab is fully live today.
    struct Def {
        const char* title;
        bool gauge;
        int warn;
    };
    static const Def defs[8] = {
        {"CPU CORE 0", true, 85},
        {"CPU CORE 1", true, 85},
        {"HEAP INTERNAL", true, 85},
        {"PSRAM", true, 85},
        {"LVGL POOL", true, 85},
        {"TASKS", false, 0},
        {"UPTIME", false, 0},
        {"MIN FREE HEAP", false, 0},
    };
    for (int i = 0; i < 8; i++) {
        sys_cards[i] = makeCard(tab,
                                kColX[i % 4],
                                kRowY[i / 4],
                                kCardW,
                                defs[i].title,
                                "esp32",
                                defs[i].gauge,
                                defs[i].warn);
    }
}

void UIDiagnosticsPage::buildLinkTab(lv_obj_t* tab) {
    static const char* titles[4] = {"LINK", "DAISY CPU", "PACKETS", "ERRORS"};
    // DAISY CPU is the only backend figure the heartbeat already carries; the
    // rest of the Daisy telemetry waits on MSG_DIAG_PUSH.
    static const bool gauge[4] = {false, true, false, false};
    for (int i = 0; i < 4; i++) {
        link_cards[i] = makeCard(tab,
                                 kColX[i % 2],
                                 kRowY[i / 2],
                                 kCardW,
                                 titles[i],
                                 i == 1 ? "wire" : "esp32",
                                 gauge[i],
                                 gauge[i] ? 60 : 0);
    }

    // Per-message-type counts. wavex_packet_stats_t already tracks these, so
    // the table needs no new plumbing at all.
    lv_obj_t* panel = lv_obj_create(tab);
    lv_obj_set_size(panel, 622, 464);
    lv_obj_set_pos(panel, kColX[2], 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColCard), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(kColBorder), 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    msg_table = lv_table_create(panel);
    lv_obj_set_size(msg_table, 620, 462);
    lv_obj_set_pos(msg_table, 0, 0);
    lv_table_set_column_count(msg_table, 2);
    lv_table_set_column_width(msg_table, 0, 430);
    lv_table_set_column_width(msg_table, 1, 180);
    lv_obj_set_style_bg_color(msg_table, lv_color_hex(kColCard), LV_PART_ITEMS);
    lv_obj_set_style_text_color(msg_table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(msg_table, &lv_font_montserrat_18, LV_PART_ITEMS);
    lv_obj_set_style_border_color(msg_table, lv_color_hex(0x222222), LV_PART_ITEMS);
    lv_obj_set_style_border_width(msg_table, 1, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(msg_table, lv_color_hex(kColCard), LV_PART_MAIN);
    lv_obj_set_style_border_width(msg_table, 0, LV_PART_MAIN);
}

void UIDiagnosticsPage::buildPendingTab(lv_obj_t* tab, const char* what) {
    char line[160];
    snprintf(line,
             sizeof(line),
             "%s telemetry lives on the Daisy and does not cross the link yet.",
             what);
    mkLabel(tab, 24, 40, line, &lv_font_montserrat_22, kColDim);
    mkLabel(tab,
            24,
            90,
            "Needs MSG_DIAG_PUSH - see docs/ui-diagnostics-spec.md",
            &lv_font_montserrat_18,
            kColDimmer);
    mkLabel(tab,
            24,
            130,
            "Showing placeholder numbers here would be worse than showing none.",
            &lv_font_montserrat_18,
            kColDimmer);
}

void UIDiagnosticsPage::onExit() {
    ESP_LOGI(TAG, "Diagnostics page exiting");

    // Stop diagnostics monitoring
    stopDiagnosticsMonitoring();

    // Clear UI element references
    ui_update_pending = false;
}

void UIDiagnosticsPage::onInput(const InputEvent& evt) {
    // Handle input events if needed
    // For now, diagnostics page is read-only
}

std::array<Softkey, NUM_SOFTKEYS> UIDiagnosticsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    keys[0] = {"Back", [this]() { UINavigator::instance().pop(); }};

    // Tab </> move the active tab. The tab bar itself stays out of the encoder
    // focus ring, so tabs are reachable without an extra focus mode.
    keys[1] = {"Tab <", [this]() {
                   active_tab = (active_tab + TAB_COUNT - 1) % TAB_COUNT;
                   if (tabview)
                       lv_tabview_set_active(tabview, active_tab, LV_ANIM_OFF);
               }};
    keys[2] = {"Tab >", [this]() {
                   active_tab = (active_tab + 1) % TAB_COUNT;
                   if (tabview)
                       lv_tabview_set_active(tabview, active_tab, LV_ANIM_OFF);
               }};
    // Freeze: values often change faster than they can be read.
    keys[3] = {"Freeze", [this]() {
                   frozen = !frozen;
                   UINavigator::instance().refreshSoftkeys();
               }};
    keys[5] = {"Samples", []() { UINavigator::instance().push(createSampleMemoryPage()); }};

    if (frozen) {
        keys[3].label = "Live";
    }
    return keys;
}

void UIDiagnosticsPage::startDiagnosticsMonitoring() {
    // Create ESP timer for data collection
    const esp_timer_create_args_t diag_timer_args = {
        .callback = &UIDiagnosticsPage::diagnosticsUpdateCallback,
        .arg = this,
        .name = "diagnostics_timer"};

    esp_err_t timer_ret = esp_timer_create(&diag_timer_args, &diagnostics_timer_handle);
    if (timer_ret == ESP_OK) {
        esp_timer_start_periodic(diagnostics_timer_handle, 500000);  // 500ms
        ESP_LOGI(TAG, "Diagnostics timer started");
    } else {
        ESP_LOGE(TAG, "Failed to create diagnostics timer: %s", esp_err_to_name(timer_ret));
    }

    // Create LVGL timer for UI updates
    lvgl_update_timer = lv_timer_create(lvglUpdateTimerCallback, 50, this);  // Check every 50ms
    if (!lvgl_update_timer) {
        ESP_LOGE(TAG, "Failed to create LVGL update timer");
    }
}

void UIDiagnosticsPage::stopDiagnosticsMonitoring() {
    // Stop and delete LVGL timer first
    if (lvgl_update_timer) {
        lv_timer_del(lvgl_update_timer);
        lvgl_update_timer = nullptr;
    }

    // Stop and delete ESP timer
    if (diagnostics_timer_handle) {
        esp_timer_stop(diagnostics_timer_handle);
        esp_timer_delete(diagnostics_timer_handle);
        diagnostics_timer_handle = nullptr;
    }

    ESP_LOGI(TAG, "Diagnostics monitoring stopped");
}

void UIDiagnosticsPage::updateCpuUsage() {
#if WAVEX_CPU_USAGE_METHOD == 1
    updateCpuUsageFreertosStats();
#elif WAVEX_CPU_USAGE_METHOD == 2
    updateCpuUsageEspIdfBuiltin();
#else
#error "Invalid WAVEX_CPU_USAGE_METHOD value. Must be 1 or 2."
#endif
}

void UIDiagnosticsPage::updateCpuUsageFreertosStats() {
    uint32_t current_time_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (last_check_time_ms == 0) {
        // Initialize previous runtime counters
        last_total_runtime = 0;
        last_idle_runtime_core0 = 0;
        last_idle_runtime_core1 = 0;
        last_system_ticks = xTaskGetTickCount();
        last_check_time_ms = current_time_ms;
        return;
    }

    uint32_t time_diff = current_time_ms - last_check_time_ms;
    if (time_diff >= 2000) {  // Update every 2 seconds (reduce frequency to avoid watchdog timeout)
        char* runtime_stats = (char*)malloc(2048);
        if (!runtime_stats) {
            ESP_LOGE(TAG, "Failed to allocate memory for runtime stats");
            return;
        }

        // vTaskGetRunTimeStats can be expensive, add a safety margin
        vTaskGetRunTimeStats(runtime_stats);

        // Parse FreeRTOS runtime statistics for multicore ESP32-P4
        // ESP32 FreeRTOS creates IDLE0, IDLE1, etc. tasks for each core
        uint32_t idle_runtime_core0 = 0;
        uint32_t idle_runtime_core1 = 0;
        uint32_t total_system_runtime = 0;
        uint32_t elapsed_ticks = 0;

        // Reset strtok for parsing
        char* saveptr = nullptr;
        char* line = strtok_r(runtime_stats, "\n", &saveptr);

        while (line != NULL) {
            // Parse each task line: "TaskName\tRuntime\t..."
            char* task_name = line;
            char* runtime_str = nullptr;

            // Find first tab (separates task name from runtime)
            char* tab = strchr(line, '\t');
            if (tab) {
                *tab = '\0';  // Null terminate task name
                runtime_str = tab + 1;

                // Parse runtime value (absolute ticks)
                uint32_t runtime = atoi(runtime_str);

                // FreeRTOS pads task names with spaces to
                // configMAX_TASK_NAME_LEN-1 (prvWriteNameToBuffer), so the
                // field reads "IDLE0          ", never "IDLE0". Comparing
                // with strcmp() never matched, both idle deltas stayed 0,
                // and every sample reported exactly 100%.
                size_t name_len = strlen(task_name);
                while (name_len > 0 && task_name[name_len - 1] == ' ') {
                    task_name[--name_len] = '\0';
                }

                // Check for per-core IDLE tasks
                if (strcmp(task_name, "IDLE0") == 0) {
                    idle_runtime_core0 = runtime;
                } else if (strcmp(task_name, "IDLE1") == 0) {
                    idle_runtime_core1 = runtime;
                }

                // Accumulate total runtime from all tasks
                total_system_runtime += runtime;
            }

            line = strtok_r(NULL, "\n", &saveptr);
        }

        // Calculate per-core CPU usage using idle time
        // CPU usage = (total_time - idle_time) / total_time * 100

        // Get elapsed system ticks for proper time measurement
        uint32_t current_ticks = xTaskGetTickCount();
        elapsed_ticks = current_ticks - last_system_ticks;
        last_system_ticks = current_ticks;

        if (total_system_runtime > last_total_runtime && last_total_runtime > 0) {
            // Calculate idle time differences
            uint32_t idle_diff_core0 = idle_runtime_core0 - last_idle_runtime_core0;
            uint32_t idle_diff_core1 = idle_runtime_core1 - last_idle_runtime_core1;
            uint32_t total_diff = total_system_runtime - last_total_runtime;

            // CPU usage calculation based on IDLE task runtime
            // CPU% = 100 - (idle_ticks / total_ticks) * 100
            if (total_diff > 0) {
                // total_diff accumulates BOTH cores, so a single core's share
                // of wall-clock time is half of it. Dividing one core's idle
                // by the two-core total (as this did) halves every idle
                // fraction, pinning a fully idle system at 50%.
                const float per_core_diff = (float)total_diff / 2.0f;
                float core0_usage = 100.0f - ((float)idle_diff_core0 / per_core_diff * 100.0f);
                float core1_usage = 100.0f - ((float)idle_diff_core1 / per_core_diff * 100.0f);

                // Overall CPU usage (average of both cores)
                cpu_usage_percent = (core0_usage + core1_usage) / 2.0f;

                // Update rolling average for stability
                cpu_usage_history[cpu_measurement_count % 10] = cpu_usage_percent;
                cpu_measurement_count++;

                float sum = 0.0f;
                int count = std::min(10, (int)cpu_measurement_count);
                for (int i = 0; i < count; i++) {
                    sum += cpu_usage_history[i];
                }
                cpu_usage_percent = sum / count;

                // Set per-core values
                cpu_usage_core0 = core0_usage;
                cpu_usage_core1 = core1_usage;

                // Clamp values to reasonable ranges
                cpu_usage_percent = std::max(0.0f, std::min(100.0f, cpu_usage_percent));
                cpu_usage_core0 = std::max(0.0f, std::min(100.0f, cpu_usage_core0));
                cpu_usage_core1 = std::max(0.0f, std::min(100.0f, cpu_usage_core1));

                if (cpu_measurement_count % 30 == 0) {
                    ESP_LOGI(TAG,
                             "FreeRTOS CPU Usage: Total=%.1f%% Core0=%.1f%% Core1=%.1f%%",
                             cpu_usage_percent,
                             cpu_usage_core0,
                             cpu_usage_core1);
                }
            }
        }

        // Store values for next iteration
        last_total_runtime = total_system_runtime;
        last_idle_runtime_core0 = idle_runtime_core0;
        last_idle_runtime_core1 = idle_runtime_core1;

        free(runtime_stats);
        last_check_time_ms = current_time_ms;
    }
}

void UIDiagnosticsPage::updateCpuUsageEspIdfBuiltin() {
    uint32_t current_time_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (last_esp_idf_check_time == 0) {
        last_esp_idf_check_time = current_time_ms;
        return;
    }

    uint32_t time_diff = current_time_ms - last_esp_idf_check_time;
    if (time_diff >= 1000) {  // Update every 1 second
        UBaseType_t task_count = uxTaskGetNumberOfTasks();
        size_t free_heap = esp_get_free_heap_size();
        size_t min_free_heap = esp_get_minimum_free_heap_size();

        // Get task list to analyze task states
        TaskStatus_t* task_status_array = nullptr;
        UBaseType_t task_status_array_size = task_count + 10;  // Extra space
        task_status_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * task_status_array_size);

        uint32_t running_tasks = 0;
        uint32_t blocked_tasks = 0;

        if (task_status_array) {
            UBaseType_t returned_task_count =
                uxTaskGetSystemState(task_status_array, task_status_array_size, nullptr);

            for (UBaseType_t i = 0; i < returned_task_count; i++) {
                eTaskState state = task_status_array[i].eCurrentState;
                if (state == eRunning || state == eReady) {
                    running_tasks++;
                } else if (state == eBlocked) {
                    blocked_tasks++;
                }
            }

            free(task_status_array);
        }

        // Calculate CPU usage based on system load indicators
        float base_load = 2.0f;  // Base system overhead

        // Task-based load (more tasks = more CPU usage)
        float task_load = (float)task_count * 0.2f;

        // Running task load (tasks that are actually executing)
        float running_load = (float)running_tasks * 1.5f;

        // Memory pressure load (low memory = more CPU usage from GC/compaction)
        float memory_load = 0.0f;
        if (free_heap < 30000) {
            memory_load = 25.0f;  // High memory pressure
        } else if (free_heap < 60000) {
            memory_load = 15.0f;  // Medium memory pressure
        } else if (free_heap < 100000) {
            memory_load = 8.0f;  // Light memory pressure
        }

        cpu_usage_percent = base_load + task_load + running_load + memory_load;

        // For ESP32-P4 (dual core), distribute usage across cores
        cpu_usage_core0 = cpu_usage_percent * 0.6f;  // Approximate core 0 usage
        cpu_usage_core1 = cpu_usage_percent * 0.4f;  // Approximate core 1 usage

        // Clamp values to reasonable ranges
        cpu_usage_percent = std::max(0.0f, std::min(100.0f, cpu_usage_percent));
        cpu_usage_core0 = std::max(0.0f, std::min(100.0f, cpu_usage_core0));
        cpu_usage_core1 = std::max(0.0f, std::min(100.0f, cpu_usage_core1));

        // Update rolling average for stability
        cpu_usage_history[cpu_measurement_count % 10] = cpu_usage_percent;
        cpu_measurement_count++;

        float sum = 0.0f;
        int count = std::min(10, (int)cpu_measurement_count);
        for (int i = 0; i < count; i++) {
            sum += cpu_usage_history[i];
        }
        cpu_usage_percent = sum / count;

        if (cpu_measurement_count % 10 == 0) {
            ESP_LOGI(TAG,
                     "ESP-IDF CPU Usage: %.1f%% (Tasks: %d running/%d total, Heap: %zu/%zu KB)",
                     cpu_usage_percent,
                     running_tasks,
                     task_count,
                     free_heap / 1024,
                     min_free_heap / 1024);
        }

        last_esp_idf_check_time = current_time_ms;
    }
}

// Timer context: sample the CPU counters (which must be read at a steady
// cadence to be meaningful) and flag the UI. Every other figure is read
// straight from its source in the per-tab refresh, on the UI task.
void UIDiagnosticsPage::collectDiagnosticsData() {
    updateCpuUsage();
    ui_update_pending = true;
    wavex_ui_mark_content_changed();
}

void UIDiagnosticsPage::applyUiUpdates() {
    if (!ui_update_pending) {
        return;
    }
    ui_update_pending = false;
    if (frozen) {
        return;  // hold the last values so a transient can be read
    }

    switch (active_tab) {
        case TAB_SYSTEM:
            refreshSystemTab();
            break;
        case TAB_LINK:
            refreshLinkTab();
            break;
        default:
            break;  // pending tabs are static until MSG_DIAG_PUSH exists
    }
}

void UIDiagnosticsPage::refreshSystemTab() {
    if (!sys_cards[0].value || !lv_obj_is_valid(sys_cards[0].value)) {
        return;
    }
    char v[48], u[32], sub[64];

    snprintf(v, sizeof(v), "%.1f", cpu_usage_core0);
    setCard(sys_cards[0], v, "%", "", (int)cpu_usage_core0);
    snprintf(v, sizeof(v), "%.1f", cpu_usage_core1);
    setCard(sys_cards[1], v, "%", "", (int)cpu_usage_core1);

    const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snprintf(v, sizeof(v), "%u", (unsigned)(heap_free / 1024));
    snprintf(sub, sizeof(sub), "of %u KB", (unsigned)(heap_total / 1024));
    setCard(sys_cards[2],
            v,
            "KB free",
            sub,
            heap_total ? (int)(100 - (heap_free * 100) / heap_total) : 0);

    const size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(v, sizeof(v), "%.1f", ps_free / (1024.0 * 1024.0));
    snprintf(sub, sizeof(sub), "of %u MB", (unsigned)(ps_total / (1024 * 1024)));
    setCard(
        sys_cards[3], v, "MB free", sub, ps_total ? (int)(100 - (ps_free * 100) / ps_total) : 0);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    snprintf(v, sizeof(v), "%u", (unsigned)((mon.total_size - mon.free_size) / 1024));
    snprintf(u, sizeof(u), "/ %u KB", (unsigned)(mon.total_size / 1024));
    snprintf(sub, sizeof(sub), "fragmentation %u%%", (unsigned)mon.frag_pct);
    setCard(sys_cards[4], v, u, sub, (int)mon.used_pct);

    snprintf(v, sizeof(v), "%u", (unsigned)uxTaskGetNumberOfTasks());
    setCard(sys_cards[5], v, "running", "", -1);

    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(v,
             sizeof(v),
             "%luh %02lum",
             (unsigned long)(up_s / 3600),
             (unsigned long)((up_s % 3600) / 60));
    setCard(sys_cards[6], v, "", "", -1);

    snprintf(v, sizeof(v), "%u", (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    setCard(sys_cards[7], v, "KB", "lowest since boot", -1);
}

void UIDiagnosticsPage::refreshLinkTab() {
    if (!link_cards[0].value || !lv_obj_is_valid(link_cards[0].value)) {
        return;
    }
    wavex_packet_stats_t st;
    inter_mcu_get_packet_stats(&st);
    wavex_backend_heartbeat_t hb;
    inter_mcu_get_backend_heartbeat_detailed(&hb);

    const uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    char v[48], sub[64];

    uint32_t age_ms = 0;
    const char* state = "NO LINK";
    if (hb.valid && hb.last_rx_ms > 0) {
        age_ms = uptime_ms - hb.last_rx_ms;
        state = (age_ms < 2000) ? "OK" : (age_ms < 5000 ? "STALE" : "LOST");
    }
    snprintf(sub,
             sizeof(sub),
             "heartbeat %lu.%lu s ago",
             (unsigned long)(age_ms / 1000),
             (unsigned long)((age_ms % 1000) / 100));
    setCard(link_cards[0], state, "", hb.valid ? sub : "no heartbeat seen", -1);

    if (hb.valid) {
        snprintf(v, sizeof(v), "%.1f", hb.cpu_avg_percent);
        snprintf(sub, sizeof(sub), "min %.1f - max %.1f", hb.cpu_min_percent, hb.cpu_max_percent);
        setCard(link_cards[1], v, "%", sub, (int)hb.cpu_avg_percent);
    } else {
        setCard(link_cards[1], "-", "%", "no heartbeat", 0);
    }

    snprintf(v, sizeof(v), "%lu", (unsigned long)st.total_packets);
    snprintf(sub,
             sizeof(sub),
             "%lu meter - %lu wave",
             (unsigned long)st.meter_push_packets,
             (unsigned long)st.wave_chunk_packets);
    setCard(link_cards[2], v, "total", sub, -1);

    snprintf(v, sizeof(v), "%lu", (unsigned long)(st.invalid_packets + st.error_packets));
    snprintf(sub,
             sizeof(sub),
             "%lu invalid - %lu error - %lu unknown",
             (unsigned long)st.invalid_packets,
             (unsigned long)st.error_packets,
             (unsigned long)st.unknown_packets);
    setCard(link_cards[3], v, "bad", sub, -1);

    if (!msg_table || !lv_obj_is_valid(msg_table)) {
        return;
    }
    struct Row {
        const char* name;
        uint32_t count;
    };
    const Row rows[] = {
        {"HEARTBEAT", st.heartbeat_packets},
        {"METER_PUSH", st.meter_push_packets},
        {"WAVE_CHUNK", st.wave_chunk_packets},
        {"STATUS_RESPONSE", st.status_response_packets},
        {"STATUS_REQUEST", st.status_request_packets},
        {"SAMPLE_CTRL", st.sample_ctrl_packets},
        {"SAMPLE_LOAD", st.sample_load_packets},
        {"SAMPLE_DATA", st.sample_data_packets},
        {"PREVIEW_REQ", st.preview_req_packets},
        {"DATA_REQUEST", st.data_request_packets},
        {"CONTROL_CHANGE", st.control_change_packets},
        {"NOTE_ON", st.note_on_packets},
        {"NOTE_OFF", st.note_off_packets},
        {"PARAMETER_UPDATE", st.parameter_update_packets},
        {"SYNC", st.sync_packets},
        {"ERROR", st.error_packets},
        {"UNKNOWN", st.unknown_packets},
        {"INVALID", st.invalid_packets},
        {"TOTAL", st.total_packets},
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0]));
    lv_table_set_row_count(msg_table, n + 1);
    lv_table_set_cell_value(msg_table, 0, 0, "MESSAGE");
    lv_table_set_cell_value(msg_table, 0, 1, "COUNT");
    for (int i = 0; i < n; i++) {
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)rows[i].count);
        lv_table_set_cell_value(msg_table, i + 1, 0, rows[i].name);
        lv_table_set_cell_value(msg_table, i + 1, 1, cnt);
    }
}

void UIDiagnosticsPage::diagnosticsUpdateCallback(void* arg) {
    UIDiagnosticsPage* page = (UIDiagnosticsPage*)arg;
    if (page) {
        page->collectDiagnosticsData();
    }
}

void UIDiagnosticsPage::lvglUpdateTimerCallback(lv_timer_t* timer) {
    UIDiagnosticsPage* page = static_cast<UIDiagnosticsPage*>(lv_timer_get_user_data(timer));
    if (page) {
        page->applyUiUpdates();
    }
}

std::shared_ptr<UIPage> createDiagnosticsPage() {
    return std::make_shared<UIDiagnosticsPage>();
}

}  // namespace wavex_ui
