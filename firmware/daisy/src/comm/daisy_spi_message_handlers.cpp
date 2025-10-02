#include "daisy_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

// Force platform define for linter
#ifndef DAISY_PLATFORM
#define DAISY_PLATFORM 1
#endif

#include "daisy_seed.h"
#include <string.h>
#include <stdint.h>
#include "config/logging_config.h" // For logging macros
#include "spi_protocol/protocol.h" // For WaveX::Protocol namespace
#include "daisy_spi_filesystem.h" // For filesystem function declarations

using namespace daisy;
using namespace WaveX::Protocol;

// Forward declaration for s_hw (defined in main file)
extern daisy::DaisySeed* s_hw;

// ============================================================================
// Message Processing Implementations
// ============================================================================

namespace WaveX {
namespace Comm {

// is_duplicate_packet is now declared in daisy_spi_link.h

// Forward declarations for message handler functions (defined later in this file)
void ProcessSyncMessage(const uint8_t* payload, size_t payload_size);
void ProcessControlChangeMessage(const uint8_t* payload, size_t payload_size);
void ProcessNoteMessage(const uint8_t* payload, size_t payload_size);
void ProcessNoteOffMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleLoadMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleControlMessage(const uint8_t* payload, size_t payload_size);
void ProcessPreviewRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessDataRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessMeterPushMessage(const uint8_t* payload, size_t payload_size);
void ProcessWaveChunkMessage(const uint8_t* payload, size_t payload_size);
void ProcessHeartbeatMessage(const uint8_t* payload, size_t payload_size);
void ProcessBrowseRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessBrowseResponseMessage(const uint8_t* payload, size_t payload_size);
void ProcessSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleStopRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleStatusMessage(const uint8_t* payload, size_t payload_size);
void ProcessSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size);
void ProcessAckMessage(const uint8_t* payload, size_t payload_size);
void ProcessErrorMessage(const uint8_t* payload, size_t payload_size);

// Message dispatcher - routes messages by type to appropriate handlers
// NOTE: Duplicate packet checking should be done at packet level, not here
void ProcessSpiMessageByType(uint8_t msg_type, uint16_t sequence_number, const uint8_t* payload, size_t payload_size)
{
    using namespace WaveX::Protocol;

    // Debug: Log message processing
    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing message - msg_type=0x%02X, seq=%u, payload_size=%d bytes",
                        msg_type, sequence_number, (int)payload_size);
        if (payload_size > 0) {
            s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                            payload[0], payload[1], payload[2], payload[3],
                            payload[4], payload[5], payload[6], payload[7]);
        }
    }

    // Route based on message type - all handlers now receive payload only
    switch (msg_type) {
        case MSG_SYNC:
            ProcessSyncMessage(payload, payload_size);
            break;
        case MSG_CONTROL_CHANGE:
            ProcessControlChangeMessage(payload, payload_size);
            break;
        case MSG_NOTE_ON:
            ProcessNoteMessage(payload, payload_size);
            break;
        case MSG_NOTE_OFF:
            ProcessNoteOffMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_LOAD:
            ProcessSampleLoadMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_CTRL:
            ProcessSampleControlMessage(payload, payload_size);
            break;
        case MSG_PREVIEW_REQ:
            ProcessPreviewRequestMessage(payload, payload_size);
            break;
        case MSG_DATA_REQUEST:
            ProcessDataRequestMessage(payload, payload_size);
            break;
        case MSG_METER_PUSH:
            ProcessMeterPushMessage(payload, payload_size);
            break;
        case MSG_WAVE_CHUNK:
            ProcessWaveChunkMessage(payload, payload_size);
            break;
        case MSG_HEARTBEAT:
            ProcessHeartbeatMessage(payload, payload_size);
            break;
        case MSG_BROWSE_REQ:
            if (s_hw) s_hw->PrintLine("DAISY: Routing MSG_BROWSE_REQ to ProcessBrowseRequestMessage");
            ProcessBrowseRequestMessage(payload, payload_size);
            break;
        case MSG_BROWSE_RESP:
            ProcessBrowseResponseMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_REQ:
            ProcessSamplePlayRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STOP_REQ:
            ProcessSampleStopRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STATUS:
            ProcessSampleStatusMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_INDEX_REQ:
            ProcessSamplePlayIndexRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_REQ:
            ProcessSampleGetPathRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_RESP:
            ProcessSampleGetPathResponseMessage(payload, payload_size);
            break;
        case MSG_ACK:
            ProcessAckMessage(payload, payload_size);
            break;
        case MSG_ERROR:
            ProcessErrorMessage(payload, payload_size);
            break;
        default:
            if (s_hw) {
                s_hw->PrintLine("DAISY: Unknown message type: 0x%02X", msg_type);
            }
            break;
    }
}

// Process browse request message
void ProcessBrowseRequestMessage(const uint8_t* payload, size_t payload_size)
{
    using namespace WaveX::Protocol;

    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessBrowseRequestMessage called - payload_size=%d", (int)payload_size);
        s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                       payload[0], payload[1], payload[2], payload[3],
                       payload[4], payload[5], payload[6], payload[7]);
    }

    // Parse browse request: start_index (1 byte) + path (null-terminated)
    uint8_t start_index = payload[0];
    const char* path_ptr = (const char*)(payload + 1);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Parsed start_index=%d, path_ptr='%s'", start_index, path_ptr);
    }
    
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

    WaveX::Comm::ProcessBrowseRequest(path, start_index, max_entries);
}

// Message processing implementations (previously stubs)
void ProcessSyncMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSyncMessage called - payload_size=%d", (int)payload_size);
}

void ProcessControlChangeMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessControlChangeMessage called - payload_size=%d", (int)payload_size);
}

void ProcessNoteMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessNoteMessage called - payload_size=%d", (int)payload_size);
}

void ProcessNoteOffMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessNoteOffMessage called - payload_size=%d", (int)payload_size);
}

void ProcessSampleLoadMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleLoadMessage called - payload_size=%d", (int)payload_size);
}

void ProcessSampleControlMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleControlMessage called - payload_size=%d", (int)payload_size);
}

void ProcessPreviewRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessPreviewRequestMessage called - payload_size=%d", (int)payload_size);
}

void ProcessDataRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessDataRequestMessage called - payload_size=%d", (int)payload_size);
}

void ProcessMeterPushMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessMeterPushMessage called - payload_size=%d", (int)payload_size);
}

void ProcessWaveChunkMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessWaveChunkMessage called - payload_size=%d", (int)payload_size);
}

void ProcessHeartbeatMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessHeartbeatMessage called - payload_size=%d", (int)payload_size);
}

void ProcessBrowseResponseMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessBrowseResponseMessage called - payload_size=%d", (int)payload_size);
}

void ProcessSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSamplePlayRequestMessage called - payload_size=%d", (int)payload_size);
    // Parse the payload to extract file path
    if (payload_size > 0) {
        const char* file_path = reinterpret_cast<const char*>(payload);
        WaveX::Comm::ProcessSamplePlayRequest(file_path);
    }
}

void ProcessSampleStopRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleStopRequestMessage called - payload_size=%d", (int)payload_size);
    WaveX::Comm::ProcessSampleStopRequest();
}

void ProcessSampleStatusMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleStatusMessage called - payload_size=%d", (int)payload_size);
}

void ProcessSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSamplePlayIndexRequestMessage called - payload_size=%d", (int)payload_size);
    
    // Parse the SamplePlayIndexMessage structure
    if (payload_size >= sizeof(WaveX::Protocol::SamplePlayIndexMessage)) {
        const WaveX::Protocol::SamplePlayIndexMessage* msg = 
            reinterpret_cast<const WaveX::Protocol::SamplePlayIndexMessage*>(payload);
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Parsed sample play index request - index=%lu", (unsigned long)msg->index);
        }
        
        WaveX::Comm::ProcessSamplePlayIndexRequest(msg->index);
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid payload size for SamplePlayIndexMessage: %d (expected %d)", 
                           (int)payload_size, (int)sizeof(WaveX::Protocol::SamplePlayIndexMessage));
        }
    }
}

void ProcessSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleGetPathRequestMessage called - payload_size=%d", (int)payload_size);
}

void ProcessSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleGetPathResponseMessage called - payload_size=%d", (int)payload_size);
}

void ProcessAckMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessAckMessage called - payload_size=%d", (int)payload_size);

    // ACK messages typically have minimal payload - just log for now
    // The actual ACK processing (sequence numbers, etc.) is handled at the packet level
    // in ProcessReceivedPacket before we get here
    
    if (s_hw) s_hw->PrintLine("DAISY: ACK message payload processing completed");
}

void ProcessErrorMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessErrorMessage called - payload_size=%d", (int)payload_size);
}

#endif // WAVEX_SPI_LINK_ENABLED

} // namespace Comm
} // namespace WaveX