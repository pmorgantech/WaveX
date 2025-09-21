#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "comm/statistics.h"
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif


// Forward declarations
class StatisticsManager;

// Inter-MCU communication interface
// This provides a high-level API for communicating with the Daisy backend
// over SPI or UART links.

// Main inter-MCU interface (simplified)
esp_err_t inter_mcu_init(void);
esp_err_t inter_mcu_start(void);

// Basic MIDI message sending
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value);
esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel);
esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel); 

// Phase I helpers
typedef enum {
    WAVEX_SAMPLE_REC_START = 1,
    WAVEX_SAMPLE_REC_STOP  = 2,
    WAVEX_SAMPLE_PLAY_START = 3,
    WAVEX_SAMPLE_PLAY_STOP  = 4,
} wavex_sample_ctrl_cmd_t;

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate);
esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim);

// Listener registration for backend->frontend messages
typedef void (*wavex_meter_cb_t)(float rms, float peak, void* user_data);
typedef void (*wavex_wave_chunk_cb_t)(uint32_t offset, const int16_t* samples, uint16_t count, void* user_data);
typedef void (*wavex_browse_resp_cb_t)(const uint8_t* data, size_t length, void* user_data);
typedef void (*wavex_sample_status_cb_t)(uint8_t state, uint32_t sample_rate, uint8_t channels, uint32_t frames_played, void* user_data);

void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data);
void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data);
void inter_mcu_set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data);
void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length);
void inter_mcu_set_sample_status_listener(wavex_sample_status_cb_t cb, void* user_data);

// Control RX task behavior
extern "C" void inter_mcu_set_suspended(bool suspended);
extern "C" bool inter_mcu_is_busy(void);
extern "C" void inter_mcu_toggle_inversion(void);

// Backend heartbeat diagnostics (from Daisy)
typedef struct {
    uint32_t uptime_ms;
    uint32_t rx_total;
    uint32_t loop_counter;
    uint32_t last_rx_ms; // esp_timer (ms) when last heartbeat was received
    float    cpu_usage_percent; // CPU usage percentage from Daisy
    bool     valid;
} wavex_backend_heartbeat_t;

// Thread-safe snapshot of latest heartbeat
void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out);

// Thread-safe snapshot of current packet statistics
void inter_mcu_get_packet_stats(wavex_packet_stats_t* out);

// Reset packet statistics (useful for testing/debugging)
void inter_mcu_reset_packet_stats(void);

// Quick packet summary (most common types)
void inter_mcu_get_packet_summary(wavex_packet_summary_t* out);

// Get current METER_PUSH packet count (useful for throttled logging)
uint32_t inter_mcu_get_meter_packet_count(void);

// Get total packet count
uint32_t inter_mcu_get_total_packet_count(void);

// Get packet statistics as formatted string (for logging/debugging)
// Returns the number of characters written (excluding null terminator)
int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size);

// Manually trigger test messages to Daisy (for debugging)
void inter_mcu_send_test_messages(void);

// Get TX statistics (messages sent to Daisy)
void inter_mcu_get_tx_stats(wavex_tx_stats_t* out);

// Update backend heartbeat statistics directly (for SPI link processing)
void inter_mcu_update_backend_heartbeat(uint32_t uptime_ms, uint32_t rx_total, uint32_t loop_counter, float cpu_usage_percent);

// Update backend meter data directly (for SPI link processing)
void inter_mcu_update_backend_meters(float rms_left, float rms_right, float peak_left, float peak_right);

// Get current meter data
void inter_mcu_get_meter_data(wavex_meter_data_t* out);

// Process packet data through the packet processor (for SPI link integration)
void inter_mcu_process_packet_data(const uint8_t* data, size_t length);

// Increment packet statistics (for SPI link integration)
void inter_mcu_increment_packet_stat(uint8_t packet_type);

// Process control messages received from Daisy (backend)
void inter_mcu_process_daisy_control_message(uint8_t type, const uint8_t* payload, uint8_t len);
