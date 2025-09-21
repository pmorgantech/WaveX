#include "inter_mcu.h"
#include "comm/link_manager.h"
#include "comm/statistics.h"
#include "comm/shared_packet_handler.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/config/link_config.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[%s] DEBUG: " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char* TAG = "InterMCU";

// Global link manager instance
static LinkManager& s_link_manager = LinkManager::getInstance();

// Statistics tracking
static StatisticsManager s_statistics;

// Task handles
#ifdef ESP_PLATFORM
static TaskHandle_t s_rx_task_handle = NULL;
static TaskHandle_t s_tx_task_handle = NULL;
#else
static void* s_rx_task_handle = NULL;
static void* s_tx_task_handle = NULL;
#endif

// Communication state
static volatile bool s_suspended = false;
static volatile bool s_initialized = false;

// Task functions
static void rx_task(void* arg);
static void tx_task(void* arg);

// Helper function declarations
static esp_err_t create_tasks();

esp_err_t inter_mcu_init()
{
    if (s_initialized) {
        ESP_LOGI(TAG, "Inter-MCU communication already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing inter-MCU communication...");
    
    // Initialize link manager (SPI only now)
    esp_err_t ret = s_link_manager.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Link manager initialization failed");
        return ret;
    }
    
    // Statistics manager doesn't need explicit initialization
    
    ESP_LOGI(TAG, "Inter-MCU communication initialized successfully");
    s_initialized = true;
    
    return ESP_OK;
}

esp_err_t inter_mcu_start()
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Inter-MCU communication not initialized");
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    ESP_LOGI(TAG, "Starting inter-MCU communication...");
    
    // Start link manager
    esp_err_t ret = s_link_manager.start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Link manager start failed");
        return ret;
    }
    
    // Start RX/TX tasks
    ret = create_tasks();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create tasks");
        return ret;
    }
    
    ESP_LOGI(TAG, "Inter-MCU communication started successfully");
    
    return ESP_OK;
}

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    return s_link_manager.send_control_change(parameter, channel, value);
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    return s_link_manager.send_note_on(note, velocity, channel);
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    return s_link_manager.send_note_off(note, channel);
}

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    return s_link_manager.send_sample_ctrl(slot, cmd, rate);
}

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    return s_link_manager.send_preview_req(slot, start, end, decim);
}

void inter_mcu_send_test_messages()
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Inter-MCU communication not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Sending test messages via %s link...", 
              s_link_manager.is_spi_link() ? "SPI" : "Unknown");
    
    s_link_manager.send_test_messages();
}

bool inter_mcu_is_busy()
{
    if (!s_initialized) {
        return false;
    }
    
    return s_link_manager.is_busy();
}

void inter_mcu_set_suspended(bool suspended)
{
    s_suspended = suspended;
    ESP_LOGI(TAG, "Inter-MCU communication %s", suspended ? "suspended" : "resumed");
}

void inter_mcu_toggle_debug()
{
    ESP_LOGI(TAG, "Debug mode toggled (SPI link only)");
}

// Implement missing functions that are declared in the header

void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data)
{
    // Store the callback in the statistics manager
    s_statistics.set_meter_callback(cb, user_data);
    ESP_LOGI(TAG, "Meter listener registered: %p", cb);
}

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data)
{
    // TODO: Implement wave chunk listener registration
    ESP_LOGI(TAG, "Wave chunk listener registration not yet implemented");
}

void inter_mcu_set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data)
{
    ESP_LOGI(TAG, "=== INTER_MCU: About to call s_statistics.set_browse_resp_callback ===");
    // Store the callback in the statistics manager
    s_statistics.set_browse_resp_callback(cb, user_data);
    ESP_LOGI(TAG, "=== INTER_MCU: Successfully called set_browse_resp_callback ===");
    ESP_LOGI(TAG, "Browse response listener registered: %p", cb);
}

void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length)
{
    ESP_LOGI(TAG, "Invoking browse response callback with %d bytes", (int)length);
    s_statistics.invoke_browse_resp_callback(data, length);
}

void inter_mcu_set_sample_status_listener(wavex_sample_status_cb_t cb, void* user_data)
{
    // Store the callback in the statistics manager
    s_statistics.set_sample_status_callback(cb, user_data);
    ESP_LOGI(TAG, "Sample status listener registered: %p", cb);
}

void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out)
{
    if (!out) {
        ESP_LOGE(TAG, "Invalid heartbeat output pointer");
        return;
    }
    
    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    float cpu_usage_percent;
    bool valid;
    s_statistics.get_backend_heartbeat(&uptime_ms, &rx_total, &loop_counter, &last_rx_ms, &cpu_usage_percent, &valid);
    
    out->uptime_ms = uptime_ms;
    out->rx_total = rx_total;
    out->loop_counter = loop_counter;
    out->last_rx_ms = last_rx_ms;
    out->cpu_usage_percent = cpu_usage_percent;
    out->valid = valid;
}

void inter_mcu_get_packet_stats(wavex_packet_stats_t* out)
{
    if (!out) {
        ESP_LOGE(TAG, "Invalid packet stats output pointer");
        return;
    }
    
    s_statistics.get_packet_stats(out);
}

void inter_mcu_reset_packet_stats(void)
{
    s_statistics.reset_packet_stats();
    ESP_LOGI(TAG, "Packet statistics reset");
}

void inter_mcu_get_packet_summary(wavex_packet_summary_t* out)
{
    if (!out) {
        ESP_LOGE(TAG, "Invalid packet summary output pointer");
        return;
    }
    
    s_statistics.get_packet_summary(out);
}

uint32_t inter_mcu_get_meter_packet_count(void)
{
    return s_statistics.get_meter_packet_count();
}

uint32_t inter_mcu_get_total_packet_count(void)
{
    return s_statistics.get_total_packet_count();
}

int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer for packet stats formatting");
        return -1;
    }
    
    return s_statistics.format_packet_stats(buffer, buffer_size);
}

void inter_mcu_get_tx_stats(wavex_tx_stats_t* out)
{
    if (!out) {
        ESP_LOGE(TAG, "Invalid TX stats output pointer");
        return;
    }
    
    s_statistics.get_tx_stats(out);
}

void inter_mcu_update_backend_heartbeat(uint32_t uptime_ms, uint32_t rx_total, uint32_t loop_counter, float cpu_usage_percent)
{
    s_statistics.update_backend_heartbeat(uptime_ms, rx_total, loop_counter, cpu_usage_percent);
}

void inter_mcu_update_backend_meters(float rms_left, float rms_right, float peak_left, float peak_right)
{
    s_statistics.update_meter_data(rms_left, rms_right, peak_left, peak_right);
}

void inter_mcu_get_meter_data(wavex_meter_data_t* out)
{
    if (!out) {
        ESP_LOGE(TAG, "Invalid meter data output pointer");
        return;
    }
    
    s_statistics.get_meter_data(out);
}

void inter_mcu_process_packet_data(const uint8_t* data, size_t length)
{
    if (!data || length == 0) {
        ESP_LOGE(TAG, "Invalid packet data for processing");
        return;
    }
    
    // TODO: Implement packet processing through the packet processor
    // For now, just increment the total packet count
    s_statistics.increment_packet_stat(0xFF); // Unknown packet type
    ESP_LOGD(TAG, "Packet data processing not yet implemented, length: %zu", length);
}

void inter_mcu_increment_packet_stat(uint8_t packet_type)
{
    s_statistics.increment_packet_stat(packet_type);
}

esp_err_t create_tasks()
{
    #ifdef ESP_PLATFORM
    // Create RX task
    BaseType_t ret = xTaskCreatePinnedToCore(rx_task, "inter_mcu_rx", 4096, NULL, 3, &s_rx_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        return -1; // ESP_FAIL
    }
    
    // Create TX task
    ret = xTaskCreatePinnedToCore(tx_task, "inter_mcu_tx", 4096, NULL, 3, &s_tx_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        return -1; // ESP_FAIL
    }
    #endif
    
    ESP_LOGI(TAG, "Inter-MCU tasks created successfully");
    return ESP_OK;
}

void stop_tasks()
{
    #ifdef ESP_PLATFORM
    if (s_rx_task_handle) {
        vTaskDelete(s_rx_task_handle);
        s_rx_task_handle = NULL;
    }
    
    if (s_tx_task_handle) {
        vTaskDelete(s_tx_task_handle);
        s_tx_task_handle = NULL;
    }
    #endif
    
    ESP_LOGI(TAG, "Inter-MCU tasks stopped");
}

// Task functions
void rx_task(void* arg)
{
    ESP_LOGI(TAG, "Inter-MCU RX task started");
    
    while (true) {
        if (s_suspended) {
            #ifdef ESP_PLATFORM
            vTaskDelay(pdMS_TO_TICKS(10));
            #endif
            continue;
        }
        
        // For SPI slave mode, packets are processed directly in the slave task
        // This RX task is mainly for future use with other link types
        // Just sleep for a while since SPI slave handles everything
        #ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay - SPI slave is self-contained
        #endif
    }
}

void tx_task(void* arg)
{
    ESP_LOGI(TAG, "Inter-MCU TX task started");
    
    while (true) {
        if (s_suspended) {
            #ifdef ESP_PLATFORM
            vTaskDelay(pdMS_TO_TICKS(10));
            #endif
            continue;
        }
        
        // TX task is mainly for future use - SPI handles most communication
        #ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(10));
        #endif
    }
}

// Process control messages received from Daisy (backend)
void inter_mcu_process_daisy_control_message(uint8_t type, const uint8_t* payload, uint8_t len)
{
#ifdef ESP_PLATFORM
    ESP_LOGI("inter_mcu", "Processing control message from Daisy: type=0x%02X, len=%d", type, len);
    
    switch (type) {
        case WaveX::Protocol::MSG_SYNC:
            // Synchronization message from Daisy - acknowledge receipt
            ESP_LOGI("inter_mcu", "Received MSG_SYNC from Daisy - acknowledging");
            // For now, just acknowledge the sync - could add sync-specific logic later
            break;
            
        case WaveX::Protocol::MSG_METER_PUSH:
            // Handle meter data from Daisy audio engine
            if (len >= 8) {
                uint16_t rms_left = payload[0] | (payload[1] << 8);
                uint16_t rms_right = payload[2] | (payload[3] << 8);
                uint16_t peak_left = payload[4] | (payload[5] << 8);
                uint16_t peak_right = payload[6] | (payload[7] << 8);
                
                // Convert Q15 to float
                float rms_l = (float)rms_left / 32767.0f;
                float rms_r = (float)rms_right / 32767.0f;
                float peak_l = (float)peak_left / 32767.0f;
                float peak_r = (float)peak_right / 32767.0f;
                
                ESP_LOGI("inter_mcu", "Meter data: RMS L=%.3f R=%.3f, Peak L=%.3f R=%.3f", 
                         rms_l, rms_r, peak_l, peak_r);
                
                // TODO: Forward to web interface or store for UI
                inter_mcu_update_backend_meters(rms_l, rms_r, peak_l, peak_r);
            }
            break;
            
        case WaveX::Protocol::MSG_HEARTBEAT:
            // Handle heartbeat from Daisy
            if (len >= 12) {
                uint32_t uptime = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
                uint32_t loop_counter = payload[4] | (payload[5] << 8) | (payload[6] << 16) | (payload[7] << 24);
                uint32_t rx_total = payload[8] | (payload[9] << 8) | (payload[10] << 16) | (payload[11] << 24);
                
                float cpu_usage = 0.0f;
                if (len >= 14) {
                    uint16_t cpu_scaled = payload[12] | (payload[13] << 8);
                    cpu_usage = (float)cpu_scaled / 10.0f;
                }
                
                ESP_LOGI("inter_mcu", "Heartbeat from Daisy: uptime=%lu, loops=%lu, rx=%lu, cpu=%.1f%%",
                         (unsigned long)uptime, (unsigned long)loop_counter, (unsigned long)rx_total, cpu_usage);
                
                inter_mcu_update_backend_heartbeat(uptime, rx_total, loop_counter, cpu_usage);
            }
            break;
            
        case WaveX::Protocol::MSG_SAMPLE_STATUS:
            // Handle sample playback status updates
            ESP_LOGI("inter_mcu", "Sample status update from Daisy");
            // TODO: Parse sample status and forward to web interface
            break;
            
        default:
            ESP_LOGW("inter_mcu", "Unknown control message type from Daisy: 0x%02X", type);
            break;
    }
#else
    (void)type;
    (void)payload;
    (void)len;
#endif
}
