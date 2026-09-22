#pragma once

#include <stddef.h>
#include <stdint.h>

#include "comm/listener_slot.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
#define taskENTER_CRITICAL(x)
#define taskEXIT_CRITICAL(x)
typedef void* portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#endif

#include "comm/statistics_types.h"

/**
 * @brief Thread-safe aggregator for link statistics, backend heartbeat, and meter data.
 *
 * Each stat group (packet counters, TX counters, heartbeat, meter data) is guarded
 * by its own spinlock, written from the UART RX task and read from the UI task. The
 * UI-facing callbacks (meter/browse/storage/sample-status) are separate from those
 * spinlocks - they go through ListenerSlot (listener_slot.h), which is mutex-guarded
 * across the invocation so a page can safely deregister while a callback may be in flight.
 */
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
    void update_backend_heartbeat(uint32_t uptime_ms,
                                  uint32_t rx_total,
                                  uint32_t loop_counter,
                                  float cpu_usage_percent);
    void update_backend_heartbeat_detailed(uint32_t uptime_ms,
                                           uint32_t rx_total,
                                           uint32_t loop_counter,
                                           float cpu_avg_percent,
                                           float cpu_min_percent,
                                           float cpu_max_percent);
    void get_backend_heartbeat(uint32_t* uptime_ms,
                               uint32_t* rx_total,
                               uint32_t* loop_counter,
                               uint32_t* last_rx_ms,
                               float* cpu_usage_percent,
                               bool* valid) const;
    void get_backend_heartbeat_detailed(uint32_t* uptime_ms,
                                        uint32_t* rx_total,
                                        uint32_t* loop_counter,
                                        uint32_t* last_rx_ms,
                                        float* cpu_avg_percent,
                                        float* cpu_min_percent,
                                        float* cpu_max_percent,
                                        bool* valid) const;

    // Meter data
    void update_meter_data(float rms_left, float rms_right, float peak_left, float peak_right);
    void get_meter_data(wavex_meter_data_t* out) const;
    void set_meter_callback(void (*callback)(float rms_left,
                                             float rms_right,
                                             float peak_left,
                                             float peak_right,
                                             void* user_data),
                            void* user_data);

    // Storage status and browse response callbacks
    void set_storage_status_callback(void (*callback)(bool mounted, void* user_data),
                                     void* user_data);
    void invoke_storage_status_callback(bool mounted);

    void set_browse_resp_callback(
        void (*callback)(const uint8_t* data, size_t length, void* user_data), void* user_data);
    void invoke_browse_resp_callback(const uint8_t* data, size_t length);

    // Sample status callback
    void set_sample_status_callback(void (*callback)(uint16_t sample_id,
                                                     uint8_t state,
                                                     uint32_t sample_rate,
                                                     uint8_t channels,
                                                     uint32_t frames_played,
                                                     void* user_data),
                                    void* user_data);
    void invoke_sample_status_callback(uint16_t sample_id,
                                       uint8_t state,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       uint32_t frames_played);

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
        float cpu_usage_percent;  // Legacy single value
        float cpu_avg_percent;    // New detailed metrics
        float cpu_min_percent;
        float cpu_max_percent;
        bool valid;
    } m_backend_hb;
    mutable portMUX_TYPE m_hb_lock;

    // Meter data
    mutable wavex_meter_data_t m_meter_data;
    mutable portMUX_TYPE m_meter_lock;

    // Listener slots. These four used to be four different disciplines - one
    // mutex held across the call, one deliberately released before it, one
    // spinlock covering only the write, and one with no locking at all - which
    // is how three of them ended up with torn-pair or use-after-free windows.
    // They are one mechanism now; see listener_slot.h.
    WaveX::Comm::ListenerSlot<void (*)(
        float rms_left, float rms_right, float peak_left, float peak_right, void* user_data)>
        m_meter_listener;

    WaveX::Comm::ListenerSlot<void (*)(const uint8_t* data, size_t length, void* user_data)>
        m_browse_resp_listener;

    WaveX::Comm::ListenerSlot<void (*)(bool mounted, void* user_data)> m_storage_status_listener;

    WaveX::Comm::ListenerSlot<void (*)(uint16_t sample_id,
                                       uint8_t state,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       uint32_t frames_played,
                                       void* user_data)>
        m_sample_status_listener;

    void update_tx_stats(uint8_t message_type);
};
