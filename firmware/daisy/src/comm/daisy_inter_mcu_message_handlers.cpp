#include "daisy_inter_mcu_message_handlers.h"

#include <string.h>

#include "audio/audio_engine.h"
#include "config/link_config.h"
#include "config/logging_config.h"
#include "config/uart_debug_config.h"
#include "daisy_filesystem.h"
#include "daisy_seed.h"
#include "daisy_uart_link.h"
#include "spi_protocol/protocol.h"

#if WAVEX_SPI_LINK_ENABLED
#include "daisy_spi_link.h"
#endif

namespace WaveX {
namespace Comm {

using namespace WaveX::Protocol;

// Forward declarations for message handler functions
static void HandleSyncMessage(const uint8_t* payload, size_t payload_size);
static void HandleControlChangeMessage(const uint8_t* payload, size_t payload_size);
static void HandleNoteMessage(const uint8_t* payload, size_t payload_size);
static void HandleNoteOffMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleLoadMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleDataMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleControlMessage(const uint8_t* payload, size_t payload_size);
static void HandlePreviewRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleDataRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleMeterPushMessage(const uint8_t* payload, size_t payload_size);
static void HandleWaveChunkMessage(const uint8_t* payload, size_t payload_size);
static void HandleHeartbeatMessage(const uint8_t* payload, size_t payload_size);
static void HandleStatusRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleBrowseRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleBrowseResponseMessage(const uint8_t* payload, size_t payload_size);
static void HandleSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleStopRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleStatusMessage(const uint8_t* payload, size_t payload_size);
static void HandleSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size);
static void HandleAckMessage(const uint8_t* payload, size_t payload_size);
static void HandleErrorMessage(const uint8_t* payload, size_t payload_size);

// Message dispatcher - transport agnostic (works with both SPI and UART)
void ProcessInterMcuMessage(uint8_t msg_type,
                            uint16_t sequence_number,
                            const uint8_t* payload,
                            size_t payload_size) {
#if WAVEX_MCU_LINK_PACKET_DEBUG
    // Per-message tracing: compile-gated (review M5 - this ran unconditionally
    // for every frame, including 20 Hz meter pushes, over blocking USB-CDC).
    if (s_hw) {
        s_hw->PrintLine(
            "DAISY: Processing message - msg_type=0x%02X, seq=%u, payload_size=%d bytes",
            msg_type,
            sequence_number,
            static_cast<int>(payload_size));
        if (payload && payload_size > 0) {
            char hex[3 * 8 + 1] = {0};
            int pos = 0;
            const size_t preview = payload_size < 8 ? payload_size : 8;
            for (size_t i = 0; i < preview; ++i) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", payload[i]);
            }
            s_hw->PrintLine("DAISY: Payload bytes: %s", hex);
        }
    }
#else
    (void)sequence_number;
#endif

    switch (msg_type) {
        case MSG_SYNC:
            HandleSyncMessage(payload, payload_size);
            break;
        case MSG_CONTROL_CHANGE:
            HandleControlChangeMessage(payload, payload_size);
            break;
        case MSG_NOTE_ON:
            HandleNoteMessage(payload, payload_size);
            break;
        case MSG_NOTE_OFF:
            HandleNoteOffMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_LOAD:
            HandleSampleLoadMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_DATA:
            HandleSampleDataMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_CTRL:
            HandleSampleControlMessage(payload, payload_size);
            break;
        case MSG_PREVIEW_REQ:
            HandlePreviewRequestMessage(payload, payload_size);
            break;
        case MSG_DATA_REQUEST:
            HandleDataRequestMessage(payload, payload_size);
            break;
        case MSG_METER_PUSH:
            HandleMeterPushMessage(payload, payload_size);
            break;
        case MSG_STATUS_REQUEST:
            HandleStatusRequestMessage(payload, payload_size);
            break;
        case MSG_WAVE_CHUNK:
            HandleWaveChunkMessage(payload, payload_size);
            break;
        case MSG_HEARTBEAT:
            HandleHeartbeatMessage(payload, payload_size);
            break;
        case MSG_BROWSE_REQ:
            HandleBrowseRequestMessage(payload, payload_size);
            break;
        case MSG_BROWSE_RESP:
            HandleBrowseResponseMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_REQ:
            HandleSamplePlayRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STOP_REQ:
            HandleSampleStopRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STATUS:
            HandleSampleStatusMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_INDEX_REQ:
            HandleSamplePlayIndexRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_REQ:
            HandleSampleGetPathRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_RESP:
            HandleSampleGetPathResponseMessage(payload, payload_size);
            break;
        case MSG_ACK:
            HandleAckMessage(payload, payload_size);
            break;
        case MSG_ERROR:
            HandleErrorMessage(payload, payload_size);
            break;
        default:
            if (s_hw) {
                s_hw->PrintLine("DAISY: Unknown message type: 0x%02X", msg_type);
            }
            break;
    }
}

// ---- Individual handler implementations ----

static void HandleSyncMessage(const uint8_t* payload, size_t payload_size) {}

// NOTE_ON/NOTE_OFF/CONTROL_CHANGE/SAMPLE_CTRL were log-only stubs until
// 2026-07-05 - the engine side (SPSC note queue -> VoiceManager) existed
// and was host-tested, but no wire message ever reached it (code review
// C1). These four now dispatch like the SAMPLE_LOAD/PREVIEW handlers
// below always did; tests/unit/comm/message_dispatch_test.cpp pins every
// routed type to its observable engine call so a stub can't silently
// reappear.

static void HandleControlChangeMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::ControlChangeMessage)) {
        UART_LOGE("daisy_msg", "CONTROL_CHANGE payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const WaveX::Protocol::ControlChangeMessage*>(payload);
    WaveX::AudioEngine::OnControlChange(*msg);
#endif
}

static void HandleNoteMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::NoteMessage)) {
        UART_LOGE("daisy_msg", "NOTE_ON payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const WaveX::Protocol::NoteMessage*>(payload);
    WaveX::AudioEngine::OnNoteOn(*msg);
#endif
}

static void HandleNoteOffMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::NoteMessage)) {
        UART_LOGE("daisy_msg", "NOTE_OFF payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    const auto* msg = reinterpret_cast<const WaveX::Protocol::NoteMessage*>(payload);
    WaveX::AudioEngine::OnNoteOff(*msg);
#endif
}

static void HandleSampleLoadMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleLoadMessage)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid payload size for SampleLoadMessage: %d (expected %d)",
                            (int)payload_size,
                            (int)sizeof(WaveX::Protocol::SampleLoadMessage));
        }
        return;
    }

    const auto* msg = reinterpret_cast<const WaveX::Protocol::SampleLoadMessage*>(payload);
    WaveX::AudioEngine::OnSampleLoad(*msg);
}

// MSG_SAMPLE_DATA (push sample bytes over the link) is not implemented:
// the engine-side receiver was unreachable dead code and was removed
// (review C2). Samples load from the Daisy's own SD card (MSG_SAMPLE_LOAD).
// The message id stays reserved in protocol.h.
static void HandleSampleDataMessage(const uint8_t* payload, size_t payload_size) {
    (void)payload;
    UART_LOGW("daisy_msg",
              "MSG_SAMPLE_DATA ignored (%d bytes) - not implemented (review C2)",
              (int)payload_size);
}

static void HandleSampleControlMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleCtrlMessage)) {
        UART_LOGE("daisy_msg", "SAMPLE_CTRL payload too small (%d)", (int)payload_size);
        return;
    }
#if WAVEX_AUDIO_ENGINE_ENABLED
    // memcpy, not reinterpret_cast: SampleCtrlMessage carries a float, and
    // the payload pointer is only byte-aligned.
    WaveX::Protocol::SampleCtrlMessage msg;
    memcpy(&msg, payload, sizeof(msg));
    WaveX::AudioEngine::OnSampleCtrl(msg);
#endif
}

static void HandlePreviewRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(PreviewReqMessage)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: PreviewReq invalid size %d (expected %d)",
                            (int)payload_size,
                            (int)sizeof(PreviewReqMessage));
        }
        return;
    }

    PreviewReqMessage req{};
    memcpy(&req, payload, sizeof(req));

    if (s_hw) {
        s_hw->PrintLine("DAISY: PreviewReq slot=%u start=%lu end=%lu decim=%u",
                        (unsigned)req.slot,
                        (unsigned long)req.start,
                        (unsigned long)req.end,
                        (unsigned)req.decim);
    }

#if WAVEX_AUDIO_ENGINE_ENABLED
    WaveX::AudioEngine::OnPreviewReq(req);
#else
    if (s_hw) {
        s_hw->PrintLine("DAISY: Audio engine disabled; cannot process preview req");
    }
#endif
}

static void HandleDataRequestMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleMeterPushMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleWaveChunkMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleHeartbeatMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleStatusRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::StatusRequestMessage)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid status request payload (size=%d)", (int)payload_size);
        }
        return;
    }

    const auto* msg = reinterpret_cast<const WaveX::Protocol::StatusRequestMessage*>(payload);
    if (msg->category == STATUS_CATEGORY_SAMPLE_MEM) {
        WaveX::Protocol::SampleMemStatusMessage status{};
        WaveX::AudioEngine::GetSampleMemStatus(status);
        WaveX::Comm::UartLinkSend(WaveX::Protocol::MSG_STATUS_RESPONSE, &status, sizeof(status));
    }
}

// Browse request handler - transport agnostic (works via SPI or UART)
// Requires filesystem support and inter-MCU communication to be enabled
static void HandleBrowseRequestMessage(const uint8_t* payload, size_t payload_size) {
    UART_LOGI("daisy_uart", "BROWSE_REQ received: payload_size=%u", (unsigned)payload_size);

    if (!payload || payload_size == 0) {
        UART_LOGE("daisy_uart", "BROWSE_REQ empty payload");
        return;
    }

    uint8_t start_index = payload[0];
    const char* path_ptr = reinterpret_cast<const char*>(payload + 1);

    if (s_hw) {
        s_hw->PrintLine("DAISY: Parsed start_index=%d, path_ptr='%s'", start_index, path_ptr);
    }
    UART_LOGI(
        "daisy_uart", "BROWSE_REQ: start_index=%u path='%s'", (unsigned)start_index, path_ptr);

    char path[96] = {0};
    size_t path_len = strlen(path_ptr);
    if (path_len >= sizeof(path)) {
        path_len = sizeof(path) - 1;
    }
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';

    uint8_t max_entries = 20;  // Default to 20 entries

    if (s_hw) {
        s_hw->PrintLine(
            "DAISY: Calling ProcessBrowseRequest with path='%s', start_index=%d, max_entries=%d",
            path,
            start_index,
            max_entries);
    }
    UART_LOGI("daisy_uart",
              "BROWSE_REQ processing: path='%s' start=%u max=%u",
              path,
              (unsigned)start_index,
              max_entries);

    WaveX::Comm::ProcessBrowseRequest(path, start_index, max_entries);
}

static void HandleBrowseResponseMessage(const uint8_t* payload, size_t payload_size) {}

// Sample play request handler - requires inter-MCU comm and audio/filesystem support
static void HandleSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (payload_size > 0 && payload) {
        const char* file_path = reinterpret_cast<const char*>(payload);
        WaveX::Comm::ProcessSamplePlayRequest(file_path);
    }
}

// Sample stop request handler - requires inter-MCU comm and audio support
static void HandleSampleStopRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleStopReqMessage)) {
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::SampleStopReqMessage*>(payload);
    WaveX::Comm::ProcessSampleStopRequest(msg->slot);
}

static void HandleSampleStatusMessage(const uint8_t* payload, size_t payload_size) {}

// Sample play index request handler - requires inter-MCU comm and audio/filesystem support
static void HandleSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(WaveX::Protocol::SamplePlayIndexMessage)) {
        if (s_hw) {
            s_hw->PrintLine(
                "DAISY: Invalid payload size for SamplePlayIndexMessage: %d (expected %d)",
                (int)payload_size,
                (int)sizeof(WaveX::Protocol::SamplePlayIndexMessage));
        }
        return;
    }

    const auto* msg = reinterpret_cast<const WaveX::Protocol::SamplePlayIndexMessage*>(payload);

    if (s_hw) {
        s_hw->PrintLine("DAISY: Parsed sample play index request - index=%lu",
                        static_cast<unsigned long>(msg->index));
    }

    WaveX::Comm::ProcessSamplePlayIndexRequest(msg->index);
}

static void HandleSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleAckMessage(const uint8_t* payload, size_t payload_size) {}

static void HandleErrorMessage(const uint8_t* payload, size_t payload_size) {}

}  // namespace Comm
}  // namespace WaveX
