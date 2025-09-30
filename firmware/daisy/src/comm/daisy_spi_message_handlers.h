#pragma once

namespace WaveX {
namespace Comm {

// Forward declarations for message processing functions moved to daisy_spi_message_handlers.cpp
// These are needed for ProcessSpiMessageByType to call them
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
void ProcessBrowseResponseMessage(const uint8_t* payload, size_t payload_size);
void ProcessSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleStopRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleStatusMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size);
void ProcessSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size);
void ProcessAckMessage(const uint8_t* payload, size_t payload_size);
void ProcessErrorMessage(const uint8_t* payload, size_t payload_size);

} // namespace Comm
} // namespace WaveX