#include "packet_processor.h"
#include "listeners.h"
#include "statistics.h"
#include "../../shared/spi_protocol/protocol.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* TAG = "PacketProcessor";
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[%s] DEBUG: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "PacketProcessor";
#endif

PacketProcessor::PacketProcessor(ListenersManager& listeners, StatisticsManager& stats)
    : m_listeners(listeners)
    , m_stats(stats)
    , m_total_rx_bytes(0)
    , m_total_raw_bytes(0)
    , m_seen_any_rx(false)
{
    memset(&m_frame_state, 0, sizeof(m_frame_state));
}

void PacketProcessor::process_data(const uint8_t* data, size_t length)
{
    if (!data || length == 0) return;
    
    m_total_raw_bytes += length;
    
    if (!m_seen_any_rx) {
        m_seen_any_rx = true;
        ESP_LOGI(TAG, "First RX: %d bytes, first=0x%02X", (int)length, data[0]);
    }
    
    // Process each byte in the incoming data
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = data[i];
        
        if (!m_frame_state.in_progress) {
            // Not in a frame - look for flexible packet type
            if (WaveX::Protocol::ProtocolHandler::IsCommandPacketType(byte) || 
                WaveX::Protocol::ProtocolHandler::IsDataPacketType(byte)) {
                m_frame_state.in_progress = true;
                m_frame_state.position = 0;
                m_frame_state.expected_total = 0;
                m_frame_state.buffer[m_frame_state.position++] = byte;
                m_frame_state.start_time = esp_timer_get_time();
            }
        } else {
            m_frame_state.buffer[m_frame_state.position++] = byte;
            
            // Once we have the packet type, compute expected total
            if (m_frame_state.expected_total == 0 && m_frame_state.position >= 1) {
                uint8_t packet_type = m_frame_state.buffer[0];
                m_frame_state.expected_total = WaveX::Protocol::ProtocolHandler::GetPacketSizeFromType(packet_type);
                
                if (m_frame_state.expected_total == 0) {
                    // Invalid packet type, reset
                    ESP_LOGW(TAG, "Invalid packet type: 0x%02X, resetting", packet_type);
                    reset_frame_state();
                    continue;
                }
                
                if (m_frame_state.expected_total > sizeof(m_frame_state.buffer)) {
                    // Invalid, reset
                    ESP_LOGW(TAG, "Frame too large: %d > %d, resetting", 
                            (int)m_frame_state.expected_total, (int)sizeof(m_frame_state.buffer));
                    reset_frame_state();
                    continue;
                }
            }
            
            // When full frame is available, validate and process
            if (m_frame_state.expected_total != 0 && m_frame_state.position >= m_frame_state.expected_total) {
                if (validate_frame(m_frame_state.buffer, m_frame_state.expected_total)) {
                    m_total_rx_bytes += m_frame_state.expected_total;
                    process_frame(m_frame_state.buffer, m_frame_state.expected_total);
                } else {
                    m_stats.increment_invalid_packet();
                    ESP_LOGW(TAG, "INVALID FRAME: checksum mismatch, searching for next sync...");
                }
                
                // Reset for next frame
                reset_frame_state();
            }
            
            // Timeout check
            uint32_t now = esp_timer_get_time();
            if (is_frame_timeout(now)) {
                ESP_LOGD(TAG, "Frame timeout after %d bytes, resyncing", (int)m_frame_state.position);
                reset_frame_state();
            }
        }
    }
}

void PacketProcessor::reset_frame_state()
{
    m_frame_state.in_progress = false;
    m_frame_state.position = 0;
    m_frame_state.expected_total = 0;
}

bool PacketProcessor::validate_frame(const uint8_t* frame_data, size_t frame_length)
{
    if (frame_length < 4) return false;
    
    uint8_t type = frame_data[1];
    uint8_t length = frame_data[2];
    uint8_t checksum = frame_data[3];
    
    if (frame_length != 4 + length) return false;
    
    uint8_t calc = WaveX::Protocol::ProtocolHandler::CalculateChecksum(&frame_data[4], length);
    return calc == checksum;
}

bool PacketProcessor::is_frame_timeout(uint32_t current_time) const
{
    return m_frame_state.in_progress && 
           (current_time - m_frame_state.start_time) > FRAME_TIMEOUT_US;
}

void PacketProcessor::process_frame(const uint8_t* frame_data, size_t frame_length)
{
    if (frame_length < 4) return;
    
    uint8_t type = frame_data[1];
    uint8_t length = frame_data[2];
    const uint8_t* payload = &frame_data[4];
    
    // Increment packet statistics
    m_stats.increment_packet_stat(type);
    
    // Log packet type with descriptive message (throttled for frequent packets)
    const char* packet_type_name = "UNKNOWN";
    bool should_log = true;
    
    switch (type) {
        case WaveX::Protocol::MSG_SYNC: packet_type_name = "SYNC"; break;
        case WaveX::Protocol::MSG_CONTROL_CHANGE: packet_type_name = "CONTROL_CHANGE"; break;
        case WaveX::Protocol::MSG_NOTE_ON: packet_type_name = "NOTE_ON"; break;
        case WaveX::Protocol::MSG_NOTE_OFF: packet_type_name = "NOTE_OFF"; break;
        case WaveX::Protocol::MSG_SAMPLE_LOAD: packet_type_name = "SAMPLE_LOAD"; break;
        case WaveX::Protocol::MSG_SAMPLE_DATA: packet_type_name = "SAMPLE_DATA"; break;
        case WaveX::Protocol::MSG_PARAMETER_UPDATE: packet_type_name = "PARAMETER_UPDATE"; break;
        case WaveX::Protocol::MSG_STATUS_REQUEST: packet_type_name = "STATUS_REQUEST"; break;
        case WaveX::Protocol::MSG_STATUS_RESPONSE: packet_type_name = "STATUS_RESPONSE"; break;
        case WaveX::Protocol::MSG_SAMPLE_CTRL: packet_type_name = "SAMPLE_CTRL"; break;
        case WaveX::Protocol::MSG_PREVIEW_REQ: packet_type_name = "PREVIEW_REQ"; break;
        case WaveX::Protocol::MSG_DATA_REQUEST: packet_type_name = "DATA_REQUEST"; break;
        case WaveX::Protocol::MSG_METER_PUSH: 
            packet_type_name = "METER_PUSH"; 
            // Only log every 100th METER_PUSH packet
            should_log = (m_stats.get_meter_packet_count() % 100 == 0);
            break;
        case WaveX::Protocol::MSG_WAVE_CHUNK: packet_type_name = "WAVE_CHUNK"; break;
        case WaveX::Protocol::MSG_HEARTBEAT: packet_type_name = "HEARTBEAT"; break;
        case WaveX::Protocol::MSG_BROWSE_RESP: packet_type_name = "BROWSE_RESP"; break;
        case WaveX::Protocol::MSG_SAMPLE_STATUS: packet_type_name = "SAMPLE_STATUS"; break;
        case WaveX::Protocol::MSG_ERROR: packet_type_name = "ERROR"; break;
        default: packet_type_name = "UNKNOWN"; break;
    }
    
    if (should_log) {
        ESP_LOGI(TAG, "Received %s packet: type=0x%02X, len=%d ✓", 
                packet_type_name, type, length);
    }
    
    // Handle specific message types
    switch (type) {
        case WaveX::Protocol::MSG_METER_PUSH:
            handle_meter_push(payload, length);
            break;
        case WaveX::Protocol::MSG_WAVE_CHUNK:
            handle_wave_chunk(payload, length);
            break;
        case WaveX::Protocol::MSG_HEARTBEAT:
            handle_heartbeat(payload, length);
            break;
        case WaveX::Protocol::MSG_SYNC:
            handle_sync(payload, length);
            break;
        case WaveX::Protocol::MSG_BROWSE_RESP:
            handle_browse_resp(payload, length);
            break;
        case WaveX::Protocol::MSG_SAMPLE_STATUS:
            handle_sample_status(payload, length);
            break;
        default:
            handle_unknown_message(type, payload, length);
            break;
    }
}

void PacketProcessor::handle_meter_push(const uint8_t* payload, size_t length)
{
    if (length == sizeof(WaveX::Protocol::MeterPushMessage)) {
        WaveX::Protocol::MeterPushMessage m;
        memset(&m, 0, sizeof(m));
        memcpy(&m, payload, sizeof(m));
        m_listeners.invoke_meter_callback(m.rms, m.peak);
    }
}

void PacketProcessor::handle_wave_chunk(const uint8_t* payload, size_t length)
{
    if (m_listeners.has_wave_chunk_listener() && length >= sizeof(WaveX::Protocol::WaveChunkMessage)) {
        WaveX::Protocol::WaveChunkMessage h;
        memset(&h, 0, sizeof(h));
        memcpy(&h, payload, sizeof(h));
        const uint16_t count = h.count;
        
        if (sizeof(WaveX::Protocol::WaveChunkMessage) + count * sizeof(int16_t) == length) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(payload + sizeof(WaveX::Protocol::WaveChunkMessage));
            m_listeners.invoke_wave_chunk_callback(h.offset, samples, count);
        }
    }
}

void PacketProcessor::handle_heartbeat(const uint8_t* payload, size_t length)
{
    if (length >= sizeof(WaveX::Protocol::HeartbeatMessage)) {
        WaveX::Protocol::HeartbeatMessage hb;
        memset(&hb, 0, sizeof(hb));
        memcpy(&hb, payload, sizeof(hb));
        
        // Convert CPU usage from scaled integer back to float
        float cpu_usage_percent = (float)hb.cpu_usage_percent / 10.0f;
        
        ESP_LOGI(TAG, "Heartbeat received: uptime=%lu, loop=%lu, cpu_raw=%u, cpu=%.1f%%", 
                 (unsigned long)hb.uptime_ms, (unsigned long)hb.loop_counter, 
                 (unsigned int)hb.cpu_usage_percent, cpu_usage_percent);
        
        m_stats.update_backend_heartbeat(hb.uptime_ms, hb.rx_total, hb.loop_counter, cpu_usage_percent);
    }
}

void PacketProcessor::handle_sync(const uint8_t* payload, size_t length)
{
    // SYNC messages don't need special handling
    (void)payload;
    (void)length;
}

void PacketProcessor::handle_browse_resp(const uint8_t* payload, size_t length)
{
    ESP_LOGI(TAG, "Processing browse response: %d bytes", (int)length);
    
    // Log raw payload for debugging (first 64 bytes)
    ESP_LOGI(TAG, "Browse response payload (first 64 bytes):");
    for (int i = 0; i < (int)length && i < 64; i++) {
        if (i % 16 == 0) {
            ESP_LOGI(TAG, "  %04X: ", i);
        }
        ESP_LOGI(TAG, "%02X ", payload[i]);
        if (i % 16 == 15) {
            ESP_LOGI(TAG, "");
        }
    }
    if (length % 16 != 0) {
        ESP_LOGI(TAG, "");
    }
    
    // Use statistics manager to invoke browse response callback
    m_stats.invoke_browse_resp_callback(payload, length);
}

void PacketProcessor::handle_sample_status(const uint8_t* payload, size_t length)
{
    if (length >= sizeof(WaveX::Protocol::SampleStatusMessage)) {
        WaveX::Protocol::SampleStatusMessage status;
        memset(&status, 0, sizeof(status));
        memcpy(&status, payload, sizeof(status));
        
        // Use statistics manager to invoke sample status callback
        m_stats.invoke_sample_status_callback(status.state, status.sample_rate, 
                                             status.channels, status.frames_played);
    }
}

void PacketProcessor::handle_unknown_message(uint8_t type, const uint8_t* payload, size_t length)
{
    ESP_LOGW(TAG, "Unknown msg type: 0x%02x", type);
    (void)payload;
    (void)length;
}
