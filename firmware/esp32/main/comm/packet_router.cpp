#include "packet_router.h"

#include "inter_mcu.h"
#include "midi_out.h"

#include <cstring>
#include <functional>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace WaveX {
namespace Comm {

using namespace WaveX::Protocol;

namespace {
PacketRouter g_packet_router_instance;

// Copies a fixed-size wire struct out of a payload region, rejecting
// null/short payloads (review H3): a CRC-valid frame with an empty payload
// used to reach memcpy(&msg, nullptr, sizeof) here - undefined behavior -
// and a truncated one filled the tail of `msg` with stale stack bytes.
template <typename T>
bool CopyMessage(const uint8_t* payload, size_t payload_len, T& out, const char* name) {
    if (!payload || payload_len < sizeof(T)) {
        ESP_LOGW("packet_router",
                 "%s payload too small (%u < %u) - dropped",
                 name,
                 (unsigned)payload_len,
                 (unsigned)sizeof(T));
        return false;
    }
    memcpy(&out, payload, sizeof(T));
    return true;
}
}  // namespace

PacketRouter& GetPacketRouter() {
    return g_packet_router_instance;
}

void PacketRouter::route_packet(const uint8_t* packet_data, size_t packet_len) {
    if (!packet_data || packet_len < 6) { // Minimum size for unified packet (4 header + 2 CRC)
        ESP_LOGW("packet_router", "Invalid packet: data=%p, len=%zu", packet_data, packet_len);
        return;
    }

    route_unified_packet(packet_data, packet_len);
}

void PacketRouter::route_uart_message(uint8_t msg_type,
                                      const uint8_t* payload,
                                      size_t payload_len,
                                      uint8_t flags,
                                      uint16_t sequence_number) {
    ESP_LOGD("packet_router",
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
    if (!WaveX::Protocol::ProtocolHandler::ValidateWaveXPacket(packet_data, packet_len)) {
        ESP_LOGE("packet_router", "Unified packet CRC validation failed");
        return;
    }

    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[2048];                  // Max payload size
    size_t payload_size = sizeof(payload);  // in: destination capacity; out: bytes copied

    if (!WaveX::Protocol::ProtocolHandler::ParseWaveXPacket(packet_data, packet_len, msg_type, payload, payload_size, sequence_number, flags)) {
        ESP_LOGE("packet_router", "Failed to parse unified packet");
        return;
    }

    ESP_LOGD(
        "packet_router",
        "Unified packet: msg_type=0x%02X, flags=0x%02X, seq=%u, payload_size=%d, total_size=%d",
        msg_type,
        flags,
        sequence_number,
        (int)payload_size,
        (int)packet_len);

    // The fixed-size envelope exposes padding as payload. This new fixed
    // message admits only zero padding; raw UART payloads stay exact-sized.
    if (msg_type == MSG_SEQ_CLOCK_OUT && payload_size > sizeof(SeqClockOutMessage)) {
        for (size_t i = sizeof(SeqClockOutMessage); i < payload_size; ++i)
            if (payload[i] != 0)
                return;
        payload_size = sizeof(SeqClockOutMessage);
    }
    route_by_message_type(msg_type, payload, payload_size, flags, sequence_number);

    if (m_stats_callback) {
        m_stats_callback(msg_type);
    }
}

void PacketRouter::route_by_message_type(uint8_t msg_type,
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         uint8_t flags,
                                         uint16_t sequence_number) {
    if (flags & PKT_FLAG_ACK) {
        ESP_LOGI(
            "packet_router", "Received ACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        // TODO: no retry queue exists yet to remove this from.
        return;
    }

    if (flags & PKT_FLAG_NACK) {
        ESP_LOGW("packet_router",
                 "Received NACK for msg_type=0x%02X, seq=%u",
                 msg_type,
                 sequence_number);
        // TODO: no retry mechanism exists yet to act on this.
        return;
    }

    switch (msg_type) {
        case MSG_SEQ_CLOCK_OUT: {
            SeqClockOutMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "SEQ_CLOCK_OUT"))
                wavex_midi::SendClock(message);
            break;
        }

        case WaveX::Protocol::MSG_SAMPLE_PLAYHEAD: {
            WaveX::Protocol::SamplePlayheadMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "SAMPLE_PLAYHEAD") &&
                WaveX::Protocol::IsValidSamplePlayhead(message))
                inter_mcu_store_sample_playhead(message);
            break;
        }
        case WaveX::Protocol::MSG_SEQ_SONG_STATUS: {
            WaveX::Protocol::SeqSongStatusMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "SEQ_SONG_STATUS") &&
                WaveX::Protocol::IsValidSeqSongStatus(message))
                inter_mcu_store_seq_song_status(message);
            break;
        }
        case WaveX::Protocol::MSG_SEQ_SLOT_STATUS: {
            WaveX::Protocol::SeqSlotStatusMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "SEQ_SLOT_STATUS") &&
                WaveX::Protocol::IsValidSeqSlotStatus(message))
                inter_mcu_store_seq_slot_status(message);
            break;
        }
        case WaveX::Protocol::MSG_BANK_STATUS: {
            WaveX::Protocol::BankStatusMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "BANK_STATUS") &&
                WaveX::Protocol::IsValidBankStatus(message))
                inter_mcu_store_bank_status(message);
        } break;
        case WaveX::Protocol::MSG_PROJECT_STATUS: {
            WaveX::Protocol::ProjectStatusMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "PROJECT_STATUS") &&
                WaveX::Protocol::IsValidProjectStatus(message))
                inter_mcu_store_project_status(message);
        } break;
        case WaveX::Protocol::MSG_CARD_STATE: {
            WaveX::Protocol::CardStateMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "CARD_STATE") &&
                WaveX::Protocol::IsValidCardState(message))
                inter_mcu_store_card_state(message);
        } break;
        case WaveX::Protocol::MSG_SEQ_FILE_STATUS: {
            WaveX::Protocol::SeqFileStatusMessage message;
            if (CopyMessage(payload, payload_len, message, "SEQ_FILE_STATUS"))
                inter_mcu_store_seq_file_status(message);
        } break;
        case WaveX::Protocol::MSG_SEQ_SLOT_PAGE: {
            WaveX::Protocol::SeqSlotPageMessage message;
            if (payload_len == sizeof(message) &&
                CopyMessage(payload, payload_len, message, "SEQ_SLOT_PAGE") &&
                WaveX::Protocol::IsValidSeqSlotPage(message))
                inter_mcu_store_seq_slot_page(message);
            break;
        }
        case WaveX::Protocol::MSG_SEQ_PATTERN_SYNC: {
            WaveX::Protocol::SeqPatternSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "SEQ_PATTERN_SYNC"))
                inter_mcu_store_seq_page(message);
        } break;
        case WaveX::Protocol::MSG_MIX_METERS: {
            WaveX::Protocol::MixMetersMessage message;
            if (CopyMessage(payload, payload_len, message, "MIX_METERS"))
                inter_mcu_store_mix_meters(message);
        } break;
        case WaveX::Protocol::MSG_MIX_STATE: {
            WaveX::Protocol::MixStateMessage message;
            if (CopyMessage(payload, payload_len, message, "MIX_STATE"))
                inter_mcu_store_mix_state(message);
        } break;
        case WaveX::Protocol::MSG_TRACK_STATE: {
            WaveX::Protocol::TrackStateMessage message;
            if (CopyMessage(payload, payload_len, message, "TRACK_STATE"))
                inter_mcu_store_track_state(message);
        } break;
        case WaveX::Protocol::MSG_INST_OSC_SYNC: {
            WaveX::Protocol::InstOscSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_OSC_SYNC"))
                inter_mcu_store_oscillator(message);
        } break;
        case WaveX::Protocol::MSG_ALLOC_SYNC: {
            WaveX::Protocol::AllocationSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "ALLOC_SYNC") &&
                WaveX::Protocol::IsValidAllocationSync(message))
                inter_mcu_store_allocation(message);
        } break;
        case WaveX::Protocol::MSG_INST_EDIT_SYNC: {
            WaveX::Protocol::InstEditSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_EDIT_SYNC"))
                inter_mcu_store_instrument_edit(message);
        } break;
        case WaveX::Protocol::MSG_INST_MOD_SYNC: {
            WaveX::Protocol::InstModSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_MOD_SYNC"))
                inter_mcu_store_modulator(message);
        } break;
        case WaveX::Protocol::MSG_INST_LFO_SYNC: {
            WaveX::Protocol::InstLfoSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_LFO_SYNC"))
                inter_mcu_store_instrument_lfo(message);
        } break;
        case WaveX::Protocol::MSG_INST_KEY_MAP_SYNC: {
            WaveX::Protocol::InstKeyMapSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_KEY_MAP_SYNC"))
                inter_mcu_store_key_map(message);
        } break;
        case WaveX::Protocol::MSG_INST_PAD_SOUND_SYNC: {
            WaveX::Protocol::InstPadSoundSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_PAD_SOUND_SYNC"))
                inter_mcu_store_pad_sound(message);
        } break;
        case WaveX::Protocol::MSG_INST_ZONE_SYNC: {
            WaveX::Protocol::InstZoneSyncMessage message;
            if (CopyMessage(payload, payload_len, message, "INST_ZONE_SYNC"))
                inter_mcu_store_instrument_map(message);
        } break;
        case WaveX::Protocol::MSG_SEQ_PLAYHEAD: {
            WaveX::Protocol::SeqPlayheadMessage message;
            if (CopyMessage(payload, payload_len, message, "SEQ_PLAYHEAD"))
                inter_mcu_store_seq_playhead(message);
        } break;
        case WaveX::Protocol::MSG_SYNC: {
            WaveX::Protocol::SyncMessage msg;
            if (CopyMessage(payload, payload_len, msg, "SYNC"))
                handle_sync(msg);
        } break;

        case WaveX::Protocol::MSG_HEARTBEAT: {
            WaveX::Protocol::HeartbeatMessage msg;
            if (CopyMessage(payload, payload_len, msg, "HEARTBEAT"))
                handle_heartbeat(msg);
        } break;

        case WaveX::Protocol::MSG_METER_PUSH: {
            WaveX::Protocol::MeterPushMessage msg;
            if (CopyMessage(payload, payload_len, msg, "METER_PUSH"))
                handle_meter_push(msg);
        } break;

        case WaveX::Protocol::MSG_STATUS_RESPONSE: {
            WaveX::Protocol::SampleMemStatusMessage msg;
            if (CopyMessage(payload, payload_len, msg, "STATUS_RESPONSE"))
                handle_status_response(msg);
        } break;

        case WaveX::Protocol::MSG_ENVELOPE_CHUNK: {
            WaveX::Protocol::EnvelopeChunkMessage msg;
            if (CopyMessage(payload, payload_len, msg, "ENVELOPE_CHUNK"))
                handle_envelope_chunk(msg, payload, payload_len);
        } break;

        case WaveX::Protocol::MSG_BROWSE_RESP: {
            // Browse responses are handled differently - they don't have message type in payload
            int64_t response_arrival_time_us = esp_timer_get_time();
            ESP_LOGD("packet_router",
                     "Browse response received: %zu bytes (t=%lld us, seq=%u)",
                     payload_len,
                     (long long)response_arrival_time_us,
                     sequence_number);
            handle_browse_resp(payload, payload_len);
            break;
        }

        case WaveX::Protocol::MSG_SAMPLE_STATUS: {
            WaveX::Protocol::SampleStatusMessage msg;
            if (CopyMessage(payload, payload_len, msg, "SAMPLE_STATUS"))
                handle_sample_status(msg);
        } break;

        case WaveX::Protocol::MSG_INST_STATUS: {
            WaveX::Protocol::InstStatusMessage msg;
            if (CopyMessage(payload, payload_len, msg, "INST_STATUS"))
                handle_inst_status(msg);
        } break;

        case WaveX::Protocol::MSG_STORAGE_STATUS: {
            WaveX::Protocol::StorageStatusMessage msg;
            if (CopyMessage(payload, payload_len, msg, "STORAGE_STATUS"))
                handle_storage_status(msg);
        } break;

        case WaveX::Protocol::MSG_SAMPLE_META: {
            WaveX::Protocol::SampleMetadata msg;
            if (CopyMessage(payload, payload_len, msg, "SAMPLE_META"))
                handle_sample_meta(msg);
        } break;

        case WaveX::Protocol::MSG_TRACK_BINDING: {
            WaveX::Protocol::TrackBindingMessage msg;
            if (CopyMessage(payload, payload_len, msg, "TRACK_BINDING"))
                handle_track_binding(msg);
        } break;

        case WaveX::Protocol::MSG_SAMPLE_META_PAGE:
            handle_sample_meta_page(payload, payload_len);
            break;

        case WaveX::Protocol::MSG_DIAG_PUSH: {
            WaveX::Protocol::DiagPushMessage msg;
            if (CopyMessage(payload, payload_len, msg, "DIAG_PUSH"))
                handle_diag_push(msg);
        } break;

        case WaveX::Protocol::MSG_SAMPLE_STOP_RESP: {
            WaveX::Protocol::SampleStopRespMessage msg;
            if (CopyMessage(payload, payload_len, msg, "SAMPLE_STOP_RESP"))
                handle_sample_stop_resp(msg);
        } break;

        case WaveX::Protocol::MSG_CV_CAL_RESP: {
            WaveX::Protocol::CvCalMessage msg;
            if (CopyMessage(payload, payload_len, msg, "CV_CAL_RESP"))
                handle_cv_cal_resp(msg);
        } break;

        case WaveX::Protocol::MSG_ERROR: {
            WaveX::Protocol::ErrorMessage msg;
            if (CopyMessage(payload, payload_len, msg, "ERROR"))
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

    ESP_LOGD("packet_router",
             "Heartbeat: uptime=%u, loop=%u, cpu=avg:%.1f%% min:%.1f%% max:%.1f%%",
             msg.uptime_ms,
             msg.loop_counter,
             cpu_avg,
             cpu_min,
             cpu_max);

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

    float rms_left = msg.rms_left / 32767.0f;
    float rms_right = msg.rms_right / 32767.0f;
    float peak_left = msg.peak_left / 32767.0f;
    float peak_right = msg.peak_right / 32767.0f;

    inter_mcu_update_backend_meters(rms_left, rms_right, peak_left, peak_right);
}

WEAK_HANDLER void PacketRouter::handle_browse_resp(const uint8_t* data, size_t length) {
    int64_t callback_start_time_us = esp_timer_get_time();
    ESP_LOGD("packet_router",
             "Browse response: %zu bytes (callback start t=%lld us)",
             length,
             (long long)callback_start_time_us);

    int64_t callback_invoke_time_us = esp_timer_get_time();
    ESP_LOGD("packet_router",
             "About to invoke browse callback (t=%lld us, since arrival=%lld us)",
             (long long)callback_invoke_time_us,
             (long long)(callback_invoke_time_us - callback_start_time_us));
    inter_mcu_invoke_browse_resp_callback(data, length);
    int64_t callback_complete_time_us = esp_timer_get_time();
    ESP_LOGD("packet_router",
             "Browse callback completed (t=%lld us, callback duration=%lld us)",
             (long long)callback_complete_time_us,
             (long long)(callback_complete_time_us - callback_invoke_time_us));
}

WEAK_HANDLER void PacketRouter::handle_status_response(
    const WaveX::Protocol::SampleMemStatusMessage& msg) {
    ESP_LOGD("packet_router",
             "Status response: category=%u samples=%u small_free=%lu large_free=%lu",
             (unsigned)msg.category,
             (unsigned)msg.sample_count,
             (unsigned long)msg.small_free_bytes,
             (unsigned long)msg.large_free_bytes);

    if (msg.category == WaveX::Protocol::STATUS_CATEGORY_SAMPLE_MEM) {
        inter_mcu_update_sample_mem_status(msg);
    }
}

WEAK_HANDLER void PacketRouter::handle_sample_status(
    const WaveX::Protocol::SampleStatusMessage& msg) {
    ESP_LOGI("packet_router",
             "Sample status: id=%u state=0x%02X sr=%lu ch=%u frames=%lu",
             (unsigned)msg.sample_id,
             msg.state,
             (unsigned long)msg.sample_rate,
             msg.channels,
             (unsigned long)msg.frames_played);
    inter_mcu_invoke_sample_status_callback(
        msg.sample_id, msg.state, msg.sample_rate, msg.channels, msg.frames_played);
}

WEAK_HANDLER void PacketRouter::handle_inst_status(const WaveX::Protocol::InstStatusMessage& msg) {
    ESP_LOGD("packet_router",
             "Instrument status: req=%lu state=%u flags=0x%02x total=%lu loaded=%lu",
             (unsigned long)msg.request_id,
             (unsigned)msg.state,
             (unsigned)msg.flags,
             (unsigned long)msg.total_bytes,
             (unsigned long)msg.loaded_bytes);
    inter_mcu_invoke_inst_status_callback(msg);
}

WEAK_HANDLER void PacketRouter::handle_storage_status(const WaveX::Protocol::StorageStatusMessage& msg) {
    ESP_LOGI("packet_router", "Storage status: mounted=%u", (unsigned)msg.mounted);
    inter_mcu_invoke_storage_status_callback(msg.mounted != 0);
}

WEAK_HANDLER void PacketRouter::handle_sample_meta(const WaveX::Protocol::SampleMetadata& msg) {
    ESP_LOGI("packet_router",
             "Sample meta: id=%u %lu frames @%lu Hz, region %lu..%lu loop %lu..%lu %s",
             (unsigned)msg.sample_id,
             (unsigned long)msg.total_frames,
             (unsigned long)msg.sample_rate,
             (unsigned long)msg.start_frame,
             (unsigned long)msg.end_frame,
             (unsigned long)msg.loop_start,
             (unsigned long)msg.loop_end,
             msg.loop_enabled ? "on" : "off");
    inter_mcu_store_sample_meta(msg);
}

WEAK_HANDLER void PacketRouter::handle_track_binding(
    const WaveX::Protocol::TrackBindingMessage& msg) {
    ESP_LOGD("packet_router",
             "Track binding: track=%u state=%u sample=%u",
             (unsigned)msg.track,
             (unsigned)msg.state,
             (unsigned)msg.sample_id);
    inter_mcu_store_track_binding(msg);
}

// One Pool page: header + n records. Validated by size before anything is
// read, since a truncated frame would otherwise hand the cache garbage.
WEAK_HANDLER void PacketRouter::handle_sample_meta_page(const uint8_t* payload, size_t length) {
    WaveX::Protocol::SampleMetaPageHeader header;
    if (!CopyMessage(payload, length, header, "SAMPLE_META_PAGE")) {
        return;
    }
    const size_t need = sizeof(header) + header.n * sizeof(WaveX::Protocol::SampleMetadata);
    if (header.n > WaveX::Protocol::MAX_SAMPLE_META_PAGE || length < need) {
        ESP_LOGW("packet_router",
                 "SAMPLE_META_PAGE n=%u but %u bytes - dropped",
                 (unsigned)header.n,
                 (unsigned)length);
        return;
    }
    inter_mcu_store_sample_meta_page(header, payload + sizeof(header));
}

WEAK_HANDLER void PacketRouter::handle_diag_push(const WaveX::Protocol::DiagPushMessage& msg) {
    // Not logged: this arrives up to 10x/s while the diagnostics page is open,
    // and logging it would cost more than the telemetry is worth.
    inter_mcu_store_diag_push(msg);
}

WEAK_HANDLER void PacketRouter::handle_sample_stop_resp(const WaveX::Protocol::SampleStopRespMessage& msg) {
    ESP_LOGI("packet_router", "Sample stop response: success=%d", msg.success);

    inter_mcu_handle_sample_stop_response(msg.success == 1);
}

WEAK_HANDLER void PacketRouter::handle_cv_cal_resp(const WaveX::Protocol::CvCalMessage& msg) {
    ESP_LOGD("packet_router", "CV cal resp: group=%u", (unsigned)msg.group);
    inter_mcu_invoke_cv_cal_callback(msg);
}

WEAK_HANDLER void PacketRouter::handle_error(const WaveX::Protocol::ErrorMessage& msg) {
    // The message text comes off the wire and is not guaranteed NUL-terminated,
    // so bound it explicitly: %s on a full 48-byte field would read past the
    // struct into the caller's stack frame.
    char text[sizeof(msg.msg) + 1];
    memcpy(text, msg.msg, sizeof(msg.msg));
    text[sizeof(msg.msg)] = '\0';
    ESP_LOGE("packet_router", "Error: code=0x%02X, message=%s", msg.code, text);
    // TODO: Implement error handling
}

WEAK_HANDLER void PacketRouter::handle_envelope_chunk(
    const WaveX::Protocol::EnvelopeChunkMessage& msg, const uint8_t* payload, size_t length) {
    using namespace WaveX::Protocol;
    const size_t values = static_cast<size_t>(msg.columns) * msg.channels;
    const size_t expected = sizeof(EnvelopeChunkMessage) + values * sizeof(EnvelopeColumn8);
    if (!IsValidEnvelopeChunk(msg) || length < expected) {
        ESP_LOGW("packet_router",
                 "Envelope chunk malformed: encoding=%u channels=%u columns=%u len=%zu",
                 (unsigned)msg.encoding,
                 (unsigned)msg.channels,
                 (unsigned)msg.columns,
                 length);
        return;
    }

    // 512-byte bounded scratch, not a whole-run buffer on the UART task stack.
    // The listener synchronously copies to its own staging before we return.
    // Cache ownership and every LVGL call remain on the UI task.
    EnvelopeColumn expanded[MAX_ENVELOPE_CHUNK_VALUES];
    for (size_t i = 0; i < values; ++i) {
        EnvelopeColumn8 encoded;
        memcpy(&encoded,
               payload + sizeof(EnvelopeChunkMessage) + i * sizeof(encoded),
               sizeof(encoded));
        if (encoded.min_sample > encoded.max_sample) {
            return;
        }
        expanded[i] = encoded.Expand();
    }
    inter_mcu_invoke_envelope_chunk_callback(msg, expanded);
}

WEAK_HANDLER void PacketRouter::handle_unknown_message(uint8_t type, const uint8_t* payload, size_t length) {
    ESP_LOGW("packet_router", "Unknown message type: 0x%02X, payload length: %zu", type, length);
    (void)payload;  // Suppress unused parameter warning
}

}  // namespace Comm
}  // namespace WaveX
