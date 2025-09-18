#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
#define taskENTER_CRITICAL(x)
#define taskEXIT_CRITICAL(x)
typedef void* portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#endif

// Packet statistics structure
typedef struct {
    uint32_t sync_packets;
    uint32_t control_change_packets;
    uint32_t note_on_packets;
    uint32_t note_off_packets;
    uint32_t sample_load_packets;
    uint32_t sample_data_packets;
    uint32_t parameter_update_packets;
    uint32_t status_request_packets;
    uint32_t status_response_packets;
    uint32_t sample_ctrl_packets;
    uint32_t preview_req_packets;
    uint32_t data_request_packets;
    uint32_t meter_push_packets;
    uint32_t wave_chunk_packets;
    uint32_t heartbeat_packets;
    uint32_t error_packets;
    uint32_t unknown_packets;
    uint32_t total_packets;
    uint32_t invalid_packets;
} wavex_packet_stats_t;

// TX statistics structure
typedef struct {
    uint32_t total_messages_sent;
    uint32_t ping_messages_sent;
    uint32_t test_messages_sent;
    uint32_t last_send_time;
} wavex_tx_stats_t;

// Packet summary structure
typedef struct {
    uint32_t total_packets;
    uint32_t meter_packets;
    uint32_t heartbeat_packets;
    uint32_t control_packets;
    uint32_t invalid_packets;
} wavex_packet_summary_t;

// Meter data structure
typedef struct {
    float rms_left;
    float rms_right;
    float peak_left;
    float peak_right;
    uint32_t last_update_ms;
    bool valid;
} wavex_meter_data_t;

// Statistics manager that handles all statistics tracking
class StatisticsManager {
public:
    StatisticsManager();
    ~StatisticsManager() = default;
    
    // Packet statistics
    void increment_packet_stat(uint8_t packet_type);
    void increment_invalid_packet();
    void get_packet_stats(wavex_packet_stats_t* out) const;
    void reset_packet_stats();
    void get_packet_summary(wavex_packet_summary_t* out) const;
    uint32_t get_meter_packet_count() const;
    uint32_t get_total_packet_count() const;
    int format_packet_stats(char* buffer, size_t buffer_size) const;
    
    // TX statistics
    void increment_tx_message(uint8_t message_type);
    void get_tx_stats(wavex_tx_stats_t* out) const;
    
    // Backend heartbeat
    void update_backend_heartbeat(uint32_t uptime_ms, uint32_t rx_total, uint32_t loop_counter);
    void get_backend_heartbeat(uint32_t* uptime_ms, uint32_t* rx_total, uint32_t* loop_counter, uint32_t* last_rx_ms, bool* valid) const;
    
    // Meter data
    void update_meter_data(float rms_left, float rms_right, float peak_left, float peak_right);
    void get_meter_data(wavex_meter_data_t* out) const;
    void set_meter_callback(void (*callback)(float rms, float peak, void* user_data), void* user_data);

private:
    // Packet statistics
    mutable wavex_packet_stats_t m_packet_stats;
    mutable portMUX_TYPE m_stats_lock;
    
    // TX statistics
    mutable wavex_tx_stats_t m_tx_stats;
    mutable portMUX_TYPE m_tx_stats_lock;
    
    // Backend heartbeat
    mutable struct {
        uint32_t uptime_ms;
        uint32_t rx_total;
        uint32_t loop_counter;
        uint32_t last_rx_ms;
        bool valid;
    } m_backend_hb;
    mutable portMUX_TYPE m_hb_lock;
    
    // Meter data
    mutable wavex_meter_data_t m_meter_data;
    mutable portMUX_TYPE m_meter_lock;
    
    // Meter callback
    void (*m_meter_callback)(float rms, float peak, void* user_data);
    void* m_meter_user_data;
    
    // Helper methods
    const char* get_packet_type_name(uint8_t packet_type) const;
    void update_tx_stats(uint8_t message_type);
};
