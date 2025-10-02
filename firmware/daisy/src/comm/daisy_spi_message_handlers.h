#pragma once

namespace WaveX {
namespace Comm {

// Main message dispatcher function
void ProcessSpiMessageByType(uint8_t msg_type, uint16_t sequence_number, const uint8_t* payload, size_t payload_size);

// Forward declarations for individual message handlers
void ProcessBrowseRequestMessage(const uint8_t* payload, size_t payload_size);
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