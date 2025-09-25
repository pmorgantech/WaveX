#include "packet_router.h"
#include "inter_mcu.h"
#include <functional>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace WaveX {
namespace Comm {

using namespace WaveX::Protocol;

void PacketRouter::route_packet(const uint8_t* packet_data, size_t packet_len)
{
    if (!packet_data || packet_len < 6) { // Minimum size for unified packet (4 header + 2 CRC)
        ESP_LOGW("packet_router", "Invalid packet: data=%p, len=%zu", packet_data, packet_len);
        return;
    }

    // Route using unified packet format
    route_unified_packet(packet_data, packet_len);
}

void PacketRouter::route_unified_packet(const uint8_t* packet_data, size_t packet_len)
{
    // Validate unified packet
    if (!WaveX::Protocol::ProtocolHandler::ValidateWaveXPacket(packet_data, packet_len)) {
        ESP_LOGE("packet_router", "Unified packet CRC validation failed");
        return;
    }

    // Extract packet info using unified format
    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[2048]; // Max payload size
    size_t payload_size;

    if (!WaveX::Protocol::ProtocolHandler::ParseWaveXPacket(packet_data, packet_len, msg_type, payload, payload_size, sequence_number, flags)) {
        ESP_LOGE("packet_router", "Failed to parse unified packet");
        return;
    }

    ESP_LOGI("packet_router", "Unified packet: msg_type=0x%02X, flags=0x%02X, seq=%u, payload_size=%d, total_size=%d",
             msg_type, flags, sequence_number, (int)payload_size, (int)packet_len);

    // Route based on message type
    route_by_message_type(msg_type, payload, payload_size, flags, sequence_number);

    // Update statistics
    if (m_stats_callback) {
        m_stats_callback(msg_type);
    }
}


void PacketRouter::route_by_message_type(uint8_t msg_type, const uint8_t* payload, size_t payload_len, uint8_t flags, uint16_t sequence_number)
{
    // Handle acknowledgment packets
    if (flags & PKT_FLAG_ACK) {
        ESP_LOGI("packet_router", "Received ACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        // Handle acknowledgment - remove from retry queue if needed
        return;
    }

    // Handle negative acknowledgment packets
    if (flags & PKT_FLAG_NACK) {
        ESP_LOGW("packet_router", "Received NACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        // Handle negative acknowledgment - retry if needed
        return;
    }

    // Route based on message type - clean and efficient
    switch (msg_type) {
        case WaveX::Protocol::MSG_SYNC:
            {
                WaveX::Protocol::SyncMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                handle_sync(msg);
            }
            break;

        case WaveX::Protocol::MSG_HEARTBEAT:
            {
                WaveX::Protocol::HeartbeatMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                handle_heartbeat(msg);
            }
            break;

        case WaveX::Protocol::MSG_METER_PUSH:
            {
                WaveX::Protocol::MeterPushMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                handle_meter_push(msg);
            }
            break;

        case WaveX::Protocol::MSG_BROWSE_RESP:
            // Browse responses are handled differently - they don't have message type in payload
            ESP_LOGI("packet_router", "Browse response received: %zu bytes", payload_len);
            handle_browse_resp(payload, payload_len);
            break;

        case WaveX::Protocol::MSG_SAMPLE_STATUS:
            {
                WaveX::Protocol::SampleStatusMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                handle_sample_status(msg);
            }
            break;

        case WaveX::Protocol::MSG_ERROR:
            {
                WaveX::Protocol::ErrorMessage msg;
                memcpy(&msg, payload, sizeof(msg));
                handle_error(msg);
            }
            break;

        default:
            ESP_LOGW("packet_router", "Unknown message type: 0x%02X", msg_type);
            break;
    }
}



void PacketRouter::route_browse_response(const uint8_t* packet_data, size_t packet_len)
{
    ESP_LOGI("packet_router", "Browse response packet");
    handle_browse_resp(packet_data, packet_len);
}

// Message handlers
void PacketRouter::handle_sync(const WaveX::Protocol::SyncMessage& msg)
{
    ESP_LOGI("packet_router", "Sync: timestamp=%u", msg.timestamp_ms);
    // TODO: Implement sync handling
}

void PacketRouter::handle_heartbeat(const WaveX::Protocol::HeartbeatMessage& msg)
{
    ESP_LOGI("packet_router", "Heartbeat: uptime=%u, loop=%u, cpu=%.1f%%", 
             msg.uptime_ms, msg.loop_counter, msg.cpu_usage_percent / 10.0f);
    
    // Update backend heartbeat data
    inter_mcu_update_backend_heartbeat(msg.uptime_ms, msg.rx_total, msg.loop_counter, 
                                       msg.cpu_usage_percent / 10.0f);
}

void PacketRouter::handle_meter_push(const WaveX::Protocol::MeterPushMessage& msg)
{
    ESP_LOGI("packet_router", "Meter: L=(%u,%u) R=(%u,%u)", 
             msg.rms_left, msg.peak_left, msg.rms_right, msg.peak_right);
    
    // Convert uint16_t to float (0-32767 -> 0.0-1.0)
    float rms_left = msg.rms_left / 32767.0f;
    float rms_right = msg.rms_right / 32767.0f;
    float peak_left = msg.peak_left / 32767.0f;
    float peak_right = msg.peak_right / 32767.0f;
    
    // Update meter data
    inter_mcu_update_backend_meters(rms_left, rms_right, peak_left, peak_right);
}

void PacketRouter::handle_browse_resp(const uint8_t* data, size_t length)
{
    ESP_LOGI("packet_router", "Browse response: %zu bytes", length);
    // TODO: Implement browse response handling
}

void PacketRouter::handle_sample_status(const WaveX::Protocol::SampleStatusMessage& msg)
{
    ESP_LOGI("packet_router", "Sample status: state=0x%02X", msg.state);
    // TODO: Implement sample status handling
}

void PacketRouter::handle_error(const WaveX::Protocol::ErrorMessage& msg)
{
    ESP_LOGE("packet_router", "Error: code=0x%02X, message=%s", msg.code, msg.msg);
    // TODO: Implement error handling
}

} // namespace Comm
} // namespace WaveX
