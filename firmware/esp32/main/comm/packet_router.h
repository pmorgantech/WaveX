#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../shared/spi_protocol/protocol.h"

#include <functional>

namespace WaveX {
namespace Comm {

// Unified packet router - single entry point for all packet processing
class PacketRouter {
   public:
    PacketRouter() = default;
    ~PacketRouter() = default;

    // Main entry point - routes packets based on format and type
    void route_packet(const uint8_t* packet_data, size_t packet_len);

    // Route already-parsed UART message payloads
    void route_uart_message(uint8_t msg_type,
                            const uint8_t* payload,
                            size_t payload_len,
                            uint8_t flags,
                            uint16_t sequence_number);

    void set_stats_callback(std::function<void(uint8_t)> callback) { m_stats_callback = callback; }

   private:
    void route_unified_packet(const uint8_t* packet_data, size_t packet_len);

    void route_by_message_type(uint8_t msg_type,
                               const uint8_t* payload,
                               size_t payload_len,
                               uint8_t flags,
                               uint16_t sequence_number);

    void route_browse_response(const uint8_t* packet_data, size_t packet_len);

    // Message handlers
    void handle_sync(const WaveX::Protocol::SyncMessage& msg);
    void handle_heartbeat(const WaveX::Protocol::HeartbeatMessage& msg);
    void handle_meter_push(const WaveX::Protocol::MeterPushMessage& msg);
    void handle_wave_chunk(const WaveX::Protocol::WaveChunkMessage& msg,
                           const uint8_t* payload,
                           size_t length);
    void handle_envelope_chunk(const WaveX::Protocol::EnvelopeChunkMessage& msg,
                               const uint8_t* payload,
                               size_t length);
    void handle_browse_resp(const uint8_t* data, size_t length);
    void handle_status_response(const WaveX::Protocol::SampleMemStatusMessage& msg);
    void handle_sample_status(const WaveX::Protocol::SampleStatusMessage& msg);
    void handle_inst_status(const WaveX::Protocol::InstStatusMessage& msg);
    void handle_storage_status(const WaveX::Protocol::StorageStatusMessage& msg);
    void handle_diag_push(const WaveX::Protocol::DiagPushMessage& msg);
    void handle_sample_meta(const WaveX::Protocol::SampleMetadata& msg);
    void handle_sample_stop_resp(const WaveX::Protocol::SampleStopRespMessage& msg);
    void handle_error(const WaveX::Protocol::ErrorMessage& msg);
    void handle_cv_cal_resp(const WaveX::Protocol::CvCalMessage& msg);
    void handle_unknown_message(uint8_t type, const uint8_t* payload, size_t length);

    std::function<void(uint8_t)> m_stats_callback;
};

}  // namespace Comm
}  // namespace WaveX
