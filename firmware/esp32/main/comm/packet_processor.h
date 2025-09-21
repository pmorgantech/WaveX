#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
typedef uint32_t esp_timer_t;
#define esp_timer_get_time() 0
#endif

// Forward declarations
class ListenersManager;
class StatisticsManager;

// Packet processor that handles incoming packet parsing and routing
class PacketProcessor {
public:
    PacketProcessor(ListenersManager& listeners, StatisticsManager& stats);
    ~PacketProcessor() = default;
    
    // Process incoming data buffer
    void process_data(const uint8_t* data, size_t length);
    
    // Reset frame state (useful for recovery)
    void reset_frame_state();
    
    // Get current frame statistics
    uint32_t get_total_rx_bytes() const { return m_total_rx_bytes; }
    uint32_t get_total_raw_bytes() const { return m_total_raw_bytes; }
    bool has_seen_any_rx() const { return m_seen_any_rx; }

private:
    // Frame processing state
    struct FrameState {
        uint8_t buffer[512];  // Frame buffer
        size_t position;      // Current position in frame
        bool in_progress;     // Frame being processed
        uint32_t start_time;  // Frame start timestamp
        size_t expected_total; // Expected total frame size
    };
    
    // Process a complete frame
    void process_frame(const uint8_t* frame_data, size_t frame_length);
    
    // Handle specific message types
    void handle_meter_push(const uint8_t* payload, size_t length);
    void handle_wave_chunk(const uint8_t* payload, size_t length);
    void handle_heartbeat(const uint8_t* payload, size_t length);
    void handle_sync(const uint8_t* payload, size_t length);
    void handle_browse_resp(const uint8_t* payload, size_t length);
    void handle_sample_status(const uint8_t* payload, size_t length);
    void handle_unknown_message(uint8_t type, const uint8_t* payload, size_t length);
    
    // Frame validation
    bool validate_frame(const uint8_t* frame_data, size_t frame_length);
    bool is_frame_timeout(uint32_t current_time) const;
    
    // References to other managers
    ListenersManager& m_listeners;
    StatisticsManager& m_stats;
    
    // Frame processing state
    FrameState m_frame_state;
    
    // Statistics
    uint32_t m_total_rx_bytes;
    uint32_t m_total_raw_bytes;
    bool m_seen_any_rx;
    
    // Constants
    static const uint32_t FRAME_TIMEOUT_US = 50000; // 50ms timeout
};
