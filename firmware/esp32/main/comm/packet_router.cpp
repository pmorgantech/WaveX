#include "packet_router.h"

#include "inter_mcu.h"

#include <cstring>
#include <functional>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace WaveX {
namespace Comm {

using namespace WaveX::Protocol;

namespace {
PacketRouter g_packet_router_instance;
}

PacketRouter& GetPacketRouter() {
    return g_packet_router_instance;
}

void PacketRouter::route_packet(const uint8_t* packet_data, size_t packet_len) {
    if (!packet_data || packet_len < 6) { // Minimum size for unified packet (4 header + 2 CRC)
        ESP_LOGW("packet_router", "Invalid packet: data=%p, len=%zu", packet_data, packet_len);
        return;
    }

    // Route using unified packet format
    route_unified_packet(packet_data, packet_len);
}

void PacketRouter::route_uart_message(uint8_t msg_type,
                                      const uint8_t* payload,
                                      size_t payload_len,
                                      uint8_t flags,
                                      uint16_t sequence_number) {
    ESP_LOGI("packet_router",
             "UART packet: msg_type=0x%02X, flags=0x%02X, seq=%u, payload_size=%d",
             msg_type,
             flags,
             sequence_number,
             static_cast<int>(payload_len));

    route_by_message_type(msg_type, payload, payload_len, flags, sequence_number);

    if (m_stats_callback) {
        m_stats_callback(msg_type);
    }
}

void PacketRouter::route_unified_packet(const uint8_t* packet_data, size_t packet_len) {
    // Validate unified packet
    if (!WaveX::Protocol::ProtocolHandler::ValidateWaveXPacket(packet_data, packet_len)) {
        ESP_LOGE("packet_router", "Unified packet CRC validation failed");
        return;
    }

    // Extract packet info using unified format
    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[2048];  // Max payload size
    size_t payload_size;

    if (!WaveX::Protocol::ProtocolHandler::ParseWaveXPacket(packet_data, packet_len, msg_type, payload, payload_size, sequence_number, flags)) {
        ESP_LOGE("packet_router", "Failed to parse unified packet");
        return;
    }

    ESP_LOGI(
        "packet_router",
        "Unified packet: msg_type=0x%02X, flags=0x%02X, seq=%u, payload_size=%d, total_size=%d",
        msg_type,
        flags,
        sequence_number,
        (int)payload_size,
        (int)packet_len);

    // Route based on message type
    route_by_message_type(msg_type, payload, payload_size, flags, sequence_number);

    // Update statistics
    if (m_stats_callback) {
        m_stats_callback(msg_type);
    }
}

void PacketRouter::route_by_message_type(uint8_t msg_type,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         uint8_t flags,
                                         uint16_t sequence_number) {
    // Handle acknowledgment packets
    if (flags & PKT_FLAG_ACK) {
        ESP_LOGI(
            "packet_router", "Received ACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        // Handle acknowledgment - remove from retry queue if needed
        return;
    }

    // Handle negative acknowledgment packets
    if (flags & PKT_FLAG_NACK) {
        ESP_LOGW("packet_router",
                 "Received NACK for msg_type=0x%02X, seq=%u",
                 msg_type,
                 sequence_number);
        // Handle negative acknowledgment - retry if needed
        return;
    }

    // Route based on message type - clean and efficient
    switch (msg_type) {
        case WaveX::Protocol::MSG_SYNC: {
            WaveX::Protocol::SyncMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_sync(msg);
        } break;

        case WaveX::Protocol::MSG_HEARTBEAT: {
            WaveX::Protocol::HeartbeatMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_heartbeat(msg);
        } break;

        case WaveX::Protocol::MSG_METER_PUSH: {
            WaveX::Protocol::MeterPushMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_meter_push(msg);
        } break;

        case WaveX::Protocol::MSG_WAVE_CHUNK: {
            WaveX::Protocol::WaveChunkMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_wave_chunk(msg, payload, payload_len);
        } break;

        case WaveX::Protocol::MSG_BROWSE_RESP:
            // Browse responses are handled differently - they don't have message type in payload
            ESP_LOGI("packet_router", "Browse response received: %zu bytes", payload_len);
            handle_browse_resp(payload, payload_len);
            break;

        case WaveX::Protocol::MSG_SAMPLE_STATUS: {
            WaveX::Protocol::SampleStatusMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_sample_status(msg);
        } break;

        case WaveX::Protocol::MSG_SAMPLE_STOP_RESP: {
            WaveX::Protocol::SampleStopRespMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_sample_stop_resp(msg);
        } break;

        case WaveX::Protocol::MSG_ERROR: {
            WaveX::Protocol::ErrorMessage msg;
            memcpy(&msg, payload, sizeof(msg));
            handle_error(msg);
        } break;

        default:
            ESP_LOGW("packet_router", "Unknown message type: 0x%02X", msg_type);
            handle_unknown_message(msg_type, payload, payload_len);
            break;
    }
}

void PacketRouter::route_browse_response(const uint8_t* packet_data, size_t packet_len) {
    ESP_LOGI("packet_router", "Browse response packet");
    handle_browse_resp(packet_data, packet_len);
}

// Message handlers
#ifdef WAVEX_TEST_BUILD
#define WEAK_HANDLER __attribute__((weak))
#else
#define WEAK_HANDLER
#endif

WEAK_HANDLER void PacketRouter::handle_sync(const WaveX::Protocol::SyncMessage& msg) {
    ESP_LOGI("packet_router", "Sync: timestamp=%u", msg.timestamp_ms);
    // TODO: Implement sync handling
}

WEAK_HANDLER void PacketRouter::handle_heartbeat(const WaveX::Protocol::HeartbeatMessage& msg) {
    float cpu_avg = msg.cpu_avg_percent / 10.0f;
    float cpu_min = msg.cpu_min_percent / 10.0f;
    float cpu_max = msg.cpu_max_percent / 10.0f;

    ESP_LOGI("packet_router",
             "Heartbeat: uptime=%u, loop=%u, cpu=avg:%.1f%% min:%.1f%% max:%.1f%%",
             msg.uptime_ms,
             msg.loop_counter,
             cpu_avg,
             cpu_min,
             cpu_max);

    // Update backend heartbeat data with detailed CPU metrics
    inter_mcu_update_backend_heartbeat_detailed(
        msg.uptime_ms, msg.rx_total, msg.loop_counter, cpu_avg, cpu_min, cpu_max);
}

WEAK_HANDLER void PacketRouter::handle_meter_push(const WaveX::Protocol::MeterPushMessage& msg) {
#ifdef WAVEX_LOG_METER_DATA
    ESP_LOGI("packet_router",
             "Meter: L=(%u,%u) R=(%u,%u)",
             msg.rms_left,
             msg.peak_left,
             msg.rms_right,
             msg.peak_right);
#endif

    // Convert uint16_t to float (0-32767 -> 0.0-1.0)
    float rms_left = msg.rms_left / 32767.0f;
    float rms_right = msg.rms_right / 32767.0f;
    float peak_left = msg.peak_left / 32767.0f;
    float peak_right = msg.peak_right / 32767.0f;

    // Update meter data
    inter_mcu_update_backend_meters(rms_left, rms_right, peak_left, peak_right);
}

WEAK_HANDLER void PacketRouter::handle_browse_resp(const uint8_t* data, size_t length) {
    ESP_LOGI("packet_router", "Browse response: %zu bytes", length);

    // // Log raw payload for debugging (first 64 bytes)
    // ESP_LOGI("packet_router", "Browse response payload (first 64 bytes):");
    // for (int i = 0; i < (int)length && i < 64; i++) {
    //     if (i % 16 == 0) {
    //         ESP_LOGI("packet_router", "  %04X: ", i);
    //     }
    //     ESP_LOGI("packet_router", "%02X ", data[i]);
    //     if (i % 16 == 15) {
    //         ESP_LOGI("packet_router", "");
    //     }
    // }
    // if (length % 16 != 0) {
    //     ESP_LOGI("packet_router", "");
    // }

    // Forward browse response to inter_mcu system for callback handling
    inter_mcu_invoke_browse_resp_callback(data, length);
}

WEAK_HANDLER void PacketRouter::handle_sample_status(const WaveX::Protocol::SampleStatusMessage& msg) {
    ESP_LOGI("packet_router", "Sample status: state=0x%02X", msg.state);
    // TODO: Implement sample status handling
}

WEAK_HANDLER void PacketRouter::handle_sample_stop_resp(const WaveX::Protocol::SampleStopRespMessage& msg) {
    ESP_LOGI("packet_router", "Sample stop response: success=%d", msg.success);

    // Forward to inter_mcu layer which can handle UI callbacks
    inter_mcu_handle_sample_stop_response(msg.success == 1);
}

WEAK_HANDLER void PacketRouter::handle_error(const WaveX::Protocol::ErrorMessage& msg) {
    ESP_LOGE("packet_router", "Error: code=0x%02X, message=%s", msg.code, msg.msg);
    // TODO: Implement error handling
}

WEAK_HANDLER void PacketRouter::handle_wave_chunk(const WaveX::Protocol::WaveChunkMessage& msg, const uint8_t* payload, size_t length) {
    ESP_LOGI("packet_router", "Wave chunk: offset=%u, count=%u", msg.offset, msg.count);

    // Validate payload size matches expected size
    size_t expected_size = sizeof(WaveX::Protocol::WaveChunkMessage) + msg.count * sizeof(int16_t);
    if (length >= expected_size) {
        const int16_t* samples =
            reinterpret_cast<const int16_t*>(payload + sizeof(WaveX::Protocol::WaveChunkMessage));

        // TODO: Forward wave chunk data to audio processing system
        // For now, just log the first few samples for debugging
        ESP_LOGD("packet_router",
                 "Wave chunk samples (first 4): %d, %d, %d, %d",
                 samples[0],
                 samples[1],
                 samples[2],
                 samples[3]);
    } else {
        ESP_LOGW("packet_router",
                 "Wave chunk payload size mismatch: expected %zu, got %zu",
                 expected_size,
                 length);
    }
}

WEAK_HANDLER void PacketRouter::handle_unknown_message(uint8_t type, const uint8_t* payload, size_t length) {
    ESP_LOGW("packet_router", "Unknown message type: 0x%02X, payload length: %zu", type, length);
    (void)payload;  // Suppress unused parameter warning
}

}  // namespace Comm
}  // namespace WaveX
