#include "daisy_inter_mcu_message_handlers.h"

#include <string.h>

#include "config/logging_config.h"
#include "spi_protocol/protocol.h"

#include "daisy_seed.h"

#if WAVEX_SPI_LINK_ENABLED
#include "daisy_spi_link.h"
#include "daisy_spi_filesystem.h"
#endif

namespace WaveX {
namespace Comm {

// Forward declaration for hardware logger (defined in main.cpp)
#if WAVEX_SPI_LINK_ENABLED
extern daisy::DaisySeed* s_hw;
#endif

#if WAVEX_SPI_LINK_ENABLED

// -----------------------------------------------------------------------------
// Existing SPI message handling implementation (transport-agnostic payload)
// -----------------------------------------------------------------------------

using namespace WaveX::Protocol;

// Forward declarations for message handler functions (defined later in this file)
static void HandleSyncMessage(const uint8_t* payload, size_t payload_size);
static void HandleControlChangeMessage(const uint8_t* payload, size_t payload_size);
static void HandleNoteMessage(const uint8_t* payload, size_t payload_size);
static void HandleNoteOffMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleLoadMessage(const uint8_t* payload, size_t payload_size);
static void HandleSampleControlMessage(const uint8_t* payload, size_t payload_size);
static void HandlePreviewRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleDataRequestMessage(const uint8_t* payload, size_t payload_size);
static void HandleMeterPushMessage(const uint8_t* payload, size_t payload_size);
static void HandleWaveChunkMessage(const uint8_t* payload, size_t payload_size);
static void HandleHeartbeatMessage(const uint8_t* payload, size_t payload_size);
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

void ProcessInterMcuMessage(uint8_t msg_type,
                            uint16_t sequence_number,
                            const uint8_t* payload,
                            size_t payload_size)
{
    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing message - msg_type=0x%02X, seq=%u, payload_size=%d bytes",
                        msg_type, sequence_number, static_cast<int>(payload_size));
        if (payload && payload_size > 0) {
            const size_t preview = payload_size < 8 ? payload_size : 8;
            switch (preview) {
                default:
                case 8:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                                    payload[0], payload[1], payload[2], payload[3],
                                    payload[4], payload[5], payload[6], payload[7]);
                    break;
                case 7:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X %02X",
                                    payload[0], payload[1], payload[2], payload[3],
                                    payload[4], payload[5], payload[6]);
                    break;
                case 6:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X",
                                    payload[0], payload[1], payload[2], payload[3],
                                    payload[4], payload[5]);
                    break;
                case 5:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X",
                                    payload[0], payload[1], payload[2], payload[3], payload[4]);
                    break;
                case 4:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X",
                                    payload[0], payload[1], payload[2], payload[3]);
                    break;
                case 3:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X",
                                    payload[0], payload[1], payload[2]);
                    break;
                case 2:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X %02X",
                                    payload[0], payload[1]);
                    break;
                case 1:
                    s_hw->PrintLine("DAISY: Payload bytes: %02X", payload[0]);
                    break;
                case 0:
                    break;
            }
        }
    }

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

static void HandleSyncMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleSyncMessage called - payload_size=%d", (int)payload_size);
}

static void HandleControlChangeMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleControlChangeMessage called - payload_size=%d", (int)payload_size);
}

static void HandleNoteMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleNoteMessage called - payload_size=%d", (int)payload_size);
}

static void HandleNoteOffMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleNoteOffMessage called - payload_size=%d", (int)payload_size);
}

static void HandleSampleLoadMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleSampleLoadMessage called - payload_size=%d", (int)payload_size);
}

static void HandleSampleControlMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleSampleControlMessage called - payload_size=%d", (int)payload_size);
}

static void HandlePreviewRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandlePreviewRequestMessage called - payload_size=%d", (int)payload_size);
}

static void HandleDataRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleDataRequestMessage called - payload_size=%d", (int)payload_size);
}

static void HandleMeterPushMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleMeterPushMessage called - payload_size=%d", (int)payload_size);
}

static void HandleWaveChunkMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleWaveChunkMessage called - payload_size=%d", (int)payload_size);
}

static void HandleHeartbeatMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleHeartbeatMessage called - payload_size=%d", (int)payload_size);
}

static void HandleBrowseRequestMessage(const uint8_t* payload, size_t payload_size)
{
    if (s_hw) {
        s_hw->PrintLine("DAISY: HandleBrowseRequestMessage called - payload_size=%d", (int)payload_size);
    }
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
    UART_LOGI("daisy_uart", "BROWSE_REQ: start_index=%u path='%s'", (unsigned)start_index, path_ptr);

    char path[96] = {0};
    size_t path_len = strlen(path_ptr);
    if (path_len >= sizeof(path)) {
        path_len = sizeof(path) - 1;
    }
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';

    uint8_t max_entries = 20; // Default to 20 entries

    if (s_hw) {
        s_hw->PrintLine("DAISY: Calling ProcessBrowseRequest with path='%s', start_index=%d, max_entries=%d",
                        path, start_index, max_entries);
    }
    UART_LOGI("daisy_uart", "BROWSE_REQ processing: path='%s' start=%u max=%u", path, (unsigned)start_index, max_entries);

    WaveX::Comm::ProcessBrowseRequest(path, start_index, max_entries);
}

static void HandleBrowseResponseMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleBrowseResponseMessage called - payload_size=%d", (int)payload_size);
}

static void HandleSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size)
{
    if (s_hw) s_hw->PrintLine("DAISY: HandleSamplePlayRequestMessage called - payload_size=%d", (int)payload_size);
    if (payload_size > 0 && payload) {
        const char* file_path = reinterpret_cast<const char*>(payload);
        WaveX::Comm::ProcessSamplePlayRequest(file_path);
    }
}

static void HandleSampleStopRequestMessage(const uint8_t* payload, size_t payload_size)
{
    if (s_hw) s_hw->PrintLine("DAISY: HandleSampleStopRequestMessage called - payload_size=%d", (int)payload_size);
    if (!payload || payload_size < sizeof(WaveX::Protocol::SampleStopReqMessage)) {
        return;
    }
    const auto* msg = reinterpret_cast<const WaveX::Protocol::SampleStopReqMessage*>(payload);
    WaveX::Comm::ProcessSampleStopRequest(msg->slot);
}

static void HandleSampleStatusMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleSampleStatusMessage called - payload_size=%d", (int)payload_size);
}

static void HandleSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size)
{
    if (s_hw) s_hw->PrintLine("DAISY: HandleSamplePlayIndexRequestMessage called - payload_size=%d", (int)payload_size);

    if (!payload || payload_size < sizeof(WaveX::Protocol::SamplePlayIndexMessage)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid payload size for SamplePlayIndexMessage: %d (expected %d)",
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

static void HandleSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleSampleGetPathRequestMessage called - payload_size=%d", (int)payload_size);
}

static void HandleSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleSampleGetPathResponseMessage called - payload_size=%d", (int)payload_size);
}

static void HandleAckMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleAckMessage called - payload_size=%d", (int)payload_size);
}

static void HandleErrorMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: HandleErrorMessage called - payload_size=%d", (int)payload_size);
}

#else  // WAVEX_SPI_LINK_ENABLED == 0

// When SPI link is disabled, we still provide the dispatcher symbol so UART can
// link successfully. For now we simply log that the message was ignored.
void ProcessInterMcuMessage(uint8_t msg_type,
                            uint16_t sequence_number,
                            const uint8_t* payload,
                            size_t payload_size)
{
    (void)msg_type;
    (void)sequence_number;
    (void)payload;
    (void)payload_size;
    // TODO: provide UART-specific implementations for browse/sample commands.
}

#endif // WAVEX_SPI_LINK_ENABLED

} // namespace Comm
} // namespace WaveX


