#include "inter_mcu.h"
#include "comm/link_manager.h"
#include "comm/packet_processor.h"
#include "comm/statistics.h"
#include "comm/listeners.h"
#include "links/uart_link.h"
#include "tasks/rx_task.h"
#include "tasks/tx_task.h"
#include "../../shared/config/link_config.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_err.h"
static const char* TAG = "inter_mcu";
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
#define esp_err_to_name(err) "Unknown Error"
static const char* TAG = "inter_mcu";
#endif

#ifdef ESP_PLATFORM

// Global instances of the new modular components
static ListenersManager s_listeners;
static StatisticsManager s_stats;
static PacketProcessor s_packet_processor(s_listeners, s_stats);
static RxTask s_rx_task(s_packet_processor, s_stats);
static TxTask s_tx_task(s_stats);
static LinkManager& s_link_manager = LinkManager::getInstance();

// Early init (now a lightweight stub). Real link bring-up occurs in inter_mcu_start().
esp_err_t inter_mcu_init(void)
{
    ESP_LOGI(TAG, "Inter-MCU init (link type: %s)", 
             s_link_manager.is_spi_link() ? "SPI" : "UART");
    
    // Initialize the link manager
    esp_err_t ret = s_link_manager.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Link manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t inter_mcu_start(void) {
    ESP_LOGI(TAG, "Starting inter-MCU link...");
    
    // Start the link manager
    esp_err_t ret = s_link_manager.start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Link manager start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Start the RX and TX tasks
    ret = s_rx_task.start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX task start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = s_tx_task.start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX task start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Inter-MCU communication started successfully");
    
    return ESP_OK;
}

// Message sending functions - delegate to link manager
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    return s_link_manager.send_control_change(parameter, channel, value);
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) {
    return s_link_manager.send_note_on(note, velocity, channel);
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) {
    return s_link_manager.send_note_off(note, channel);
}

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) {
    return s_link_manager.send_sample_ctrl(slot, static_cast<uint8_t>(cmd), rate);
}

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) {
    return s_link_manager.send_preview_req(slot, start, end, decim);
}

// Listener registration - delegate to listeners manager
void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data) {
    s_listeners.set_meter_listener(cb, user_data);
}

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) {
    s_listeners.set_wave_chunk_listener(cb, user_data);
}

// Control RX task behavior
extern "C" void inter_mcu_set_suspended(bool suspended) {
    s_rx_task.set_suspended(suspended);
    
    // Also set suspended on UART link if it's the current link
    if (s_link_manager.is_uart_link()) {
        UartLink* uart_link = static_cast<UartLink*>(s_link_manager.get_current_link());
        if (uart_link) {
            uart_link->set_suspended(suspended);
        }
    }
}

extern "C" bool inter_mcu_is_busy(void) {
    return s_link_manager.is_busy();
}

extern "C" void inter_mcu_toggle_inversion(void) {
    if (s_link_manager.is_uart_link()) {
        UartLink* uart_link = static_cast<UartLink*>(s_link_manager.get_current_link());
        if (uart_link) {
            uart_link->toggle_inversion();
        }
    }
}

// Backend heartbeat diagnostics - delegate to statistics manager
void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out) {
    if (!out) return;
    
    uint32_t uptime_ms, rx_total, loop_counter, last_rx_ms;
    bool valid;
    s_stats.get_backend_heartbeat(&uptime_ms, &rx_total, &loop_counter, &last_rx_ms, &valid);
    
    out->uptime_ms = uptime_ms;
    out->rx_total = rx_total;
    out->loop_counter = loop_counter;
    out->last_rx_ms = last_rx_ms;
    out->valid = valid;
}

// Packet statistics - delegate to statistics manager
void inter_mcu_get_packet_stats(wavex_packet_stats_t* out) {
    s_stats.get_packet_stats(out);
}

void inter_mcu_reset_packet_stats(void) {
    s_stats.reset_packet_stats();
}

void inter_mcu_get_packet_summary(wavex_packet_summary_t* out) {
    s_stats.get_packet_summary(out);
}

uint32_t inter_mcu_get_meter_packet_count(void) {
    return s_stats.get_meter_packet_count();
}

uint32_t inter_mcu_get_total_packet_count(void) {
    return s_stats.get_total_packet_count();
}

int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size) {
    return s_stats.format_packet_stats(buffer, buffer_size);
}

// Manually trigger test messages - delegate to link manager
void inter_mcu_send_test_messages(void) {
    s_link_manager.send_test_messages();
}

// Get TX statistics - delegate to statistics manager
void inter_mcu_get_tx_stats(wavex_tx_stats_t* out) {
    s_stats.get_tx_stats(out);
}

#else // !ESP_PLATFORM (host/lint build stubs)

// Minimal stubs for non-ESP builds so linters and host builds pass
esp_err_t inter_mcu_init(void) { return ESP_OK; }
esp_err_t inter_mcu_start(void) { return ESP_OK; }
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) { (void)parameter; (void)channel; (void)value; return ESP_OK; }
esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) { (void)note; (void)velocity; (void)channel; return ESP_OK; }
esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) { (void)note; (void)channel; return ESP_OK; }
esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) { (void)slot; (void)cmd; (void)rate; return ESP_OK; }
esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) { (void)slot; (void)start; (void)end; (void)decim; return ESP_OK; }
void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data) { (void)cb; (void)user_data; }
void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) { (void)cb; (void)user_data; }
extern "C" void inter_mcu_set_suspended(bool suspended) { (void)suspended; }
extern "C" bool inter_mcu_is_busy(void) { return false; }
extern "C" void inter_mcu_toggle_inversion(void) {}
void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out) { if (out) memset(out, 0, sizeof(*out)); }
void inter_mcu_get_packet_stats(wavex_packet_stats_t* out) { if (out) memset(out, 0, sizeof(*out)); }
void inter_mcu_reset_packet_stats(void) {}
void inter_mcu_get_packet_summary(wavex_packet_summary_t* out) { if (out) memset(out, 0, sizeof(*out)); }
uint32_t inter_mcu_get_meter_packet_count(void) { return 0; }
uint32_t inter_mcu_get_total_packet_count(void) { return 0; }
int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size) { if (buffer && buffer_size > 0) buffer[0] = '\0'; return 0; }
void inter_mcu_send_test_messages(void) {}
void inter_mcu_get_tx_stats(wavex_tx_stats_t* out) { if (out) memset(out, 0, sizeof(*out)); }

#endif // ESP_PLATFORM
