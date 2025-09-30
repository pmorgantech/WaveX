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
// Message Processing Stub Implementations
// ============================================================================

namespace WaveX {
namespace Comm {

// Message processing stub implementations (non-static so they can be called from main file)
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