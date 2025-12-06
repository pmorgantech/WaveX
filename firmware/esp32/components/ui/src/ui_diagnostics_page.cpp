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
      diagnostics_label(nullptr),
      daisy_label(nullptr),
      ui_update_pending(false) {
    memset(cpu_usage_history, 0, sizeof(cpu_usage_history));
    memset(deferred_esp32_text, 0, sizeof(deferred_esp32_text));
    memset(deferred_daisy_text, 0, sizeof(deferred_daisy_text));
}

UIDiagnosticsPage::~UIDiagnosticsPage() {
    // Cleanup is done in onExit()
}

const char* UIDiagnosticsPage::name() const {
    return "Diagnostics";
}

void UIDiagnosticsPage::onEnter(lv_obj_t* parent) {
    ESP_LOGI(TAG, "Diagnostics page entering");

    // Clean the parent
    lv_obj_clean(parent);

    // Initialize UI elements
    diagnostics_label = nullptr;
    daisy_label = nullptr;

    // Create main content container
    lv_obj_t* content = lv_obj_create(parent);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(content, lv_color_make(0x00, 0x00, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(content, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(content, 8, LV_PART_MAIN);

    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // ESP32 Status Column
    createEsp32StatusColumn(content);

    // Daisy Link Column
    createDaisyStatusColumn(content);

    // Meters Column
    createMetersColumn(content);

    // Start diagnostics monitoring
    startDiagnosticsMonitoring();
}

void UIDiagnosticsPage::onExit() {
    ESP_LOGI(TAG, "Diagnostics page exiting");

    // Stop diagnostics monitoring
    stopDiagnosticsMonitoring();

    // Clear UI element references
    diagnostics_label = nullptr;
    daisy_label = nullptr;
    ui_update_pending = false;
}

void UIDiagnosticsPage::onInput(const InputEvent& evt) {
    // Handle input events if needed
    // For now, diagnostics page is read-only
}

std::array<Softkey, NUM_SOFTKEYS> UIDiagnosticsPage::getSoftkeys() {
    std::array<Softkey, NUM_SOFTKEYS> keys{};

    // Back button
    keys[0] = {"Back", [this]() { UINavigator::instance().pop(); }};

    // Sample memory diagnostics
    keys[1] = {"Samples", []() { UINavigator::instance().push(createSampleMemoryPage()); }};

    return keys;
}

void UIDiagnosticsPage::createEsp32StatusColumn(lv_obj_t* parent) {
    lv_obj_t* esp32_column = lv_obj_create(parent);
    lv_obj_set_size(esp32_column, lv_pct(30), lv_pct(100));
    lv_obj_set_style_bg_color(esp32_column, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(esp32_column, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(esp32_column, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_pad_all(esp32_column, 10, LV_PART_MAIN);

    lv_obj_t* esp32_title = lv_label_create(esp32_column);
    lv_label_set_text(esp32_title, "ESP32 Status");
    lv_obj_set_style_text_font(esp32_title, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(esp32_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(esp32_title, LV_ALIGN_TOP_MID, 0, 5);

    diagnostics_label = lv_label_create(esp32_column);
    char esp32_text[512];
    snprintf(esp32_text,
             sizeof(esp32_text),
             "Uptime: 0 sec\n"
             "Free RAM: %zu KB\n"
             "Min RAM: %zu KB\n"
             "CPU: ESP32-P4\n"
             "CPU Total: 0.0%%\n"
             "CPU Core 0: 0.0%%\n"
             "CPU Core 1: 0.0%%",
             (size_t)(esp_get_free_heap_size() / 1024),
             (size_t)(esp_get_minimum_free_heap_size() / 1024));
    lv_label_set_text(diagnostics_label, esp32_text);
    lv_obj_set_style_text_font(diagnostics_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(diagnostics_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(diagnostics_label, LV_ALIGN_TOP_LEFT, 0, 30);
}

void UIDiagnosticsPage::createDaisyStatusColumn(lv_obj_t* parent) {
    lv_obj_t* daisy_column = lv_obj_create(parent);
    lv_obj_set_size(daisy_column, lv_pct(30), lv_pct(100));
    lv_obj_set_style_bg_color(daisy_column, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(daisy_column, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(daisy_column, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_pad_all(daisy_column, 10, LV_PART_MAIN);

    lv_obj_t* daisy_title = lv_label_create(daisy_column);
    lv_label_set_text(daisy_title, "Daisy Link");
    lv_obj_set_style_text_font(daisy_title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(daisy_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(daisy_title, LV_ALIGN_TOP_MID, 0, 5);

    daisy_label = lv_label_create(daisy_column);
    char daisy_text[512];
    snprintf(daisy_text,
             sizeof(daisy_text),
             "Status: CHECKING...\n"
             "Last RX: -- ms ago\n"
             "Total Packets: 0\n"
             "Heartbeat: 0\n"
             "Meter Packets: 0\n"
             "CRC Errors: 0\n"
             "IRQ Count: 0\n"
             "SPI Active: NO\n"
             "Daisy CPU: 0.0%%");
    lv_label_set_text(daisy_label, daisy_text);
    lv_obj_set_style_text_font(daisy_label, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(daisy_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(daisy_label, LV_ALIGN_TOP_LEFT, 0, 30);
}

void UIDiagnosticsPage::createMetersColumn(lv_obj_t* parent) {
    lv_obj_t* meters_column = lv_obj_create(parent);
    lv_obj_set_size(meters_column, lv_pct(35), lv_pct(100));
    lv_obj_set_style_bg_color(meters_column, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(meters_column, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(meters_column, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_pad_all(meters_column, 10, LV_PART_MAIN);

    wavex_ui_create_meter_display(meters_column);
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
                float core0_usage = 100.0f - ((float)idle_diff_core0 / total_diff * 100.0f);
                float core1_usage = 100.0f - ((float)idle_diff_core1 / total_diff * 100.0f);

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

void UIDiagnosticsPage::collectDiagnosticsData() {
    updateCpuUsage();

    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

    wavex_backend_heartbeat_t heartbeat;
    inter_mcu_get_backend_heartbeat_detailed(&heartbeat);

#if WAVEX_SPI_LINK_ENABLED
    spi_link_stats_t spi_stats;
    spi_link_get_stats(&spi_stats);
    bool spi_active = spi_link_is_active();
#else
    struct {
        uint32_t packets_sent, packets_received, crc_errors, irq_count, rx_pool_empty,
            last_activity_ms;
    } spi_stats = {};
    bool spi_active = false;
#endif

    wavex_packet_stats_t packet_stats;
    inter_mcu_get_packet_stats(&packet_stats);

    uint32_t time_since_last_rx = 999999;
    if (heartbeat.valid && heartbeat.last_rx_ms > 0) {
        time_since_last_rx = uptime_ms - heartbeat.last_rx_ms;
    }

    const char* link_status = "INACTIVE";
    if (heartbeat.valid && time_since_last_rx < 2000) {
        link_status = "ACTIVE";
    } else if (heartbeat.valid && time_since_last_rx < 5000) {
        link_status = "STALE";
    } else if (spi_stats.packets_received > 0) {
        link_status = "PARTIAL";
    } else if (spi_active) {
        link_status = "INIT";
    }

    // Prepare text data
    int esp32_len = snprintf(deferred_esp32_text,
                             sizeof(deferred_esp32_text),
                             "Uptime: %lu sec\n"
                             "Free RAM: %zu KB\n"
                             "Min RAM: %zu KB\n"
                             "CPU: ESP32-P4\n"
                             "CPU Total: %.1f%%\n"
                             "CPU Core 0: %.1f%%\n"
                             "CPU Core 1: %.1f%%",
                             uptime_ms / 1000,
                             free_heap / 1024,
                             min_free_heap / 1024,
                             cpu_usage_percent,
                             cpu_usage_core0,
                             cpu_usage_core1);

    int daisy_len = snprintf(deferred_daisy_text,
                             sizeof(deferred_daisy_text),
                             "Status: %s\n"
                             "Last RX: %lu ms ago\n"
                             "Total Packets: %lu\n"
                             "Heartbeat: %lu\n"
                             "Meter Packets: %lu\n"
                             "CRC Errors: %lu\n"
                             "IRQ Count: %lu\n"
                             "SPI Active: %s\n"
                             "Daisy CPU: avg=%.1f%% min=%.1f%% max=%.1f%%",
                             link_status,
                             time_since_last_rx,
                             packet_stats.total_packets,
                             packet_stats.heartbeat_packets,
                             packet_stats.meter_push_packets,
                             spi_stats.crc_errors,
                             spi_stats.irq_count,
                             spi_active ? "YES" : "NO",
                             heartbeat.cpu_avg_percent,
                             heartbeat.cpu_min_percent,
                             heartbeat.cpu_max_percent);

    // Safety check
    if (esp32_len >= sizeof(deferred_esp32_text) || daisy_len >= sizeof(deferred_daisy_text)) {
        ESP_LOGE(TAG, "Buffer overflow in diagnostics text formatting");
        return;
    }

    ui_update_pending = true;
    wavex_ui_mark_content_changed();
}

void UIDiagnosticsPage::applyUiUpdates() {
    if (!ui_update_pending) {
        return;
    }

    if (!diagnostics_label || !daisy_label) {
        return;
    }

    if (lv_obj_is_valid(diagnostics_label)) {
        lv_label_set_text(diagnostics_label, deferred_esp32_text);
    }
    if (lv_obj_is_valid(daisy_label)) {
        lv_label_set_text(daisy_label, deferred_daisy_text);
    }

    ui_update_pending = false;
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
