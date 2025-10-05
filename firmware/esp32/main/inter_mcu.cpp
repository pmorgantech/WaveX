#include "inter_mcu.h"
#include "comm/statistics.h"
#include "comm/shared_packet_handler.h"
#include "links/esp_spi_link.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/config/link_config.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include <vector>
#include "esp_timer.h"
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

// Direct SPI link state
static bool s_spi_initialized = false;
static bool s_spi_started = false;

// Statistics tracking
static StatisticsManager s_statistics;

// Communication state
static volatile bool s_suspended = false;
static volatile bool s_initialized = false;

esp_err_t inter_mcu_init()
{
    if (s_initialized) {
        ESP_LOGI(TAG, "Inter-MCU communication already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing inter-MCU communication...");
    
    // Initialize SPI link directly
    esp_err_t ret = spi_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI link initialization failed");
        return ret;
    }
    
    s_spi_initialized = true;
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
    
    // Start SPI link directly
    esp_err_t ret = spi_link_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI link start failed");
        return ret;
    }
    
    s_spi_started = true;

    ESP_LOGI(TAG, "Inter-MCU communication started successfully");
    
    return ESP_OK;
}

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::ControlChangeMessage msg;
    msg.parameter = parameter;
    msg.channel = channel;
    msg.value = value;
    
    int result = spi_link_send(WaveX::Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = velocity;
    msg.channel = channel;
    msg.reserved = 0;
    
    int result = spi_link_send(WaveX::Protocol::MSG_NOTE_ON, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::NoteMessage msg;
    msg.note = note;
    msg.velocity = 0; // Note off
    msg.channel = channel;
    msg.reserved = 0;
    
    int result = spi_link_send(WaveX::Protocol::MSG_NOTE_OFF, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::SampleCtrlMessage msg;
    msg.slot = slot;
    msg.cmd = cmd;
    msg.rate = rate;
    
    int result = spi_link_send(WaveX::Protocol::MSG_SAMPLE_CTRL, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim)
{
    if (!s_initialized || s_suspended) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::PreviewReqMessage msg;
    msg.slot = slot;
    msg.start = start;
    msg.end = end;
    msg.decim = decim;
    
    int result = spi_link_send(WaveX::Protocol::MSG_PREVIEW_REQ, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

void inter_mcu_send_test_messages()
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Inter-MCU communication not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Sending test messages via SPI link...");
    
    // Send a simple control change test message
    WaveX::Protocol::ControlChangeMessage msg;
    msg.parameter = WaveX::Protocol::PARAM_VOLUME;
    msg.channel = 0;
    msg.value = 100;
    
    int result = spi_link_send(WaveX::Protocol::MSG_CONTROL_CHANGE, &msg, sizeof(msg));
    if (result) {
        ESP_LOGI(TAG, "Test message sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send test message");
    }
}

bool inter_mcu_is_busy()
{
    if (!s_initialized) {
        return false;
    }
    
    // For now, SPI link doesn't have a busy state, so return false
    // This could be enhanced to check SPI transaction queue status
    return false;
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

// Process control messages received from Daisy (backend)
void inter_mcu_process_daisy_control_message(uint8_t type, const uint8_t* payload, uint8_t len)
{
#ifdef ESP_PLATFORM
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

// Direct SPI API functions (replacing LinkManager)

esp_err_t inter_mcu_send_browse_req(const char* path, uint8_t start_index)
{
    ESP_LOGI("inter_mcu", "DEBUG - inter_mcu_send_browse_req called: path='%s', start_index=%d", path ? path : "NULL", start_index);
    
    if (!s_initialized) {
        ESP_LOGE("inter_mcu", "inter_mcu not initialized");
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    if (!path) {
        ESP_LOGE("inter_mcu", "path is NULL");
        return -1;
    }
    
    // Flexible payload: [start_index][path bytes...][\0]
    const size_t path_len = strlen(path);
    const size_t payload_len = 1 + path_len + 1; // start_index + path + null terminator
    
    std::vector<uint8_t> payload(payload_len, 0);
    payload[0] = start_index;
    memcpy(&payload[1], path, path_len);
    payload[payload_len - 1] = '\0';
    
    // Debug: Log what we're sending (limit output)
    ESP_LOGI("inter_mcu", "DEBUG - Sending browse request: start_index=%d, path='%s'", start_index, path);
    ESP_LOGI("inter_mcu", "DEBUG - Payload length: %d", (int)payload_len);
    for (size_t i = 0; i < payload_len && i < 24; i++) {
        ESP_LOGD("inter_mcu", "  [%d] = 0x%02X", (int)i, payload[i]);
    }
    
    ESP_LOGI("inter_mcu", "DEBUG - About to call spi_link_send with MSG_BROWSE_REQ=0x%02X", WaveX::Protocol::MSG_BROWSE_REQ);
    int result = spi_link_send(WaveX::Protocol::MSG_BROWSE_REQ, payload.data(), (uint16_t)payload_len);
    ESP_LOGI("inter_mcu", "DEBUG - spi_link_send returned: %d", result);
    
    return result ? ESP_OK : -1; // ESP_FAIL
}

esp_err_t inter_mcu_send_sample_play_index_req(uint32_t file_index)
{
    if (!s_initialized) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::SamplePlayIndexMessage msg;
    msg.index = file_index;
    
    int result = spi_link_send(WaveX::Protocol::MSG_SAMPLE_PLAY_INDEX_REQ, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

esp_err_t inter_mcu_send_sample_stop_req()
{
    if (!s_initialized) {
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    WaveX::Protocol::SampleStopReqMessage msg;
    msg.slot = 0; // Currently single slot; extend when multi-slot is supported
    msg.reserved[0] = 0;
    msg.reserved[1] = 0;
    msg.reserved[2] = 0;
    
    int result = spi_link_send(WaveX::Protocol::MSG_SAMPLE_STOP_REQ, &msg, sizeof(msg));
    return result ? ESP_OK : -1; // ESP_FAIL
}

// Handle sample stop response from communication layer
void inter_mcu_handle_sample_stop_response(bool success)
{
    // Forward to UI layer - this is the proper place for UI callbacks
    extern void wavex_ui_handle_sample_stop_response(bool success);
    wavex_ui_handle_sample_stop_response(success);
}

