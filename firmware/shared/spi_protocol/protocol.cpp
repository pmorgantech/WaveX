#include "protocol.h"

#include <string.h>

#include <cstdio>

namespace WaveX {
namespace Protocol {

// Outbound sequence counter - one per firmware image, so each MCU numbers
// its own transmit stream independently.
static uint16_t s_next_seq_num = 1;  // Start from 1, 0 is reserved

size_t ProtocolHandler::GetPacketSizeFromCode(uint8_t size_code) {
    switch (size_code & PKT_SIZE_MASK) {
        case PKT_SIZE_32:
            return 32;
        case PKT_SIZE_64:
            return 64;
        case PKT_SIZE_128:
            return 128;
        case PKT_SIZE_256:
            return 256;
        case PKT_SIZE_512:
            return 512;
        case PKT_SIZE_1024:
            return 1024;
        case PKT_SIZE_2048:
            return 2048;
        default:
            return 0;  // Invalid size code
    }
}

// Smallest size class whose capacity (class - 4-byte header - 2-byte CRC)
// fits the payload; saturates to PKT_SIZE_2048 (CreateWaveXPacket rejects
// what then still doesn't fit).
uint8_t ProtocolHandler::GetOptimalSizeCode(size_t payload_size) {
    if (payload_size <= 26)
        return PKT_SIZE_32;  // 32-4-2 = 26 bytes payload
    if (payload_size <= 58)
        return PKT_SIZE_64;  // 64-4-2 = 58 bytes payload
    if (payload_size <= 122)
        return PKT_SIZE_128;  // 128-4-2 = 122 bytes payload
    if (payload_size <= 250)
        return PKT_SIZE_256;  // 256-4-2 = 250 bytes payload
    if (payload_size <= 506)
        return PKT_SIZE_512;  // 512-4-2 = 506 bytes payload
    if (payload_size <= 1018)
        return PKT_SIZE_1024;  // 1024-4-2 = 1018 bytes payload
    if (payload_size <= 2042)
        return PKT_SIZE_2048;  // 2048-4-2 = 2042 bytes payload
    return PKT_SIZE_2048;      // Maximum 2048-byte packets (2048-4-2 = 2042 bytes payload)
}

// CRC-16-CCITT (polynomial 0x1021, init 0xFFFF), matching the Daisy's
// hardware CRC configuration. Both MCUs must compute this identically.
uint16_t ProtocolHandler::CalculateWaveXCrc(const uint8_t* data, size_t length) {
    if (!data || length == 0)
        return 0;

    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)(data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

// Legacy CRC calculation functions (kept for compatibility)
uint16_t ProtocolHandler::CalculateSpiCrc(const uint8_t* data, size_t length) {
    return CalculateWaveXCrc(data, length);
}

uint16_t ProtocolHandler::CalculatePacketCrc(const uint8_t* packet_data, size_t packet_size) {
    // Same size_t underflow ValidateWaveXPacket guards below: a packet_size
    // under 2 makes this subtraction wrap to ~SIZE_MAX and the CRC pass walk
    // off the buffer. Guarded here too rather than only at the caller,
    // because both of these are public entry points on ProtocolHandler.
    if (!packet_data || packet_size < sizeof(uint16_t)) {
        return 0;
    }
    // Calculate CRC over entire packet except last 2 bytes (CRC field)
    return CalculateWaveXCrc(packet_data, packet_size - sizeof(uint16_t));
}

bool ProtocolHandler::ValidateWaveXPacket(const uint8_t* buffer, size_t buffer_size) {
    // Guard the subtractions below: a 4-byte header + 2-byte CRC is the
    // smallest possible frame, and buffer_size < 2 would underflow size_t
    // (CRC over ~SIZE_MAX bytes, out-of-bounds reads of buffer[size - 2]).
    if (!buffer || buffer_size < 4 + 2) {
        return false;
    }
    uint16_t calculated_crc = CalculateWaveXCrc(buffer, buffer_size - 2);
    uint16_t received_crc = buffer[buffer_size - 2] | (buffer[buffer_size - 1] << 8);
    return calculated_crc == received_crc;
}

// Core packet creation with automatic sequence number management
size_t ProtocolHandler::CreatePacket(uint8_t* buffer,
                                     size_t buffer_size,
                                     uint8_t msg_type,
                                     const void* payload,
                                     size_t payload_size,
                                     uint8_t flags) {
    // Get next sequence number and increment it. 0 is reserved (receivers
    // reject it - see sequence_tracker.hpp), so skip it on uint16 wrap.
    uint16_t seq_num = s_next_seq_num++;
    if (seq_num == 0) {
        seq_num = s_next_seq_num++;
    }

    return CreateWaveXPacket(buffer, buffer_size, msg_type, payload, payload_size, seq_num, flags);
}

size_t ProtocolHandler::CreateWaveXPacket(uint8_t* buffer,
                                          size_t buffer_size,
                                          uint8_t msg_type,
                                          const void* payload,
                                          size_t payload_size,
                                          uint16_t sequence_number,
                                          uint8_t flags) {
    uint8_t size_code = GetOptimalSizeCode(payload_size);
    size_t total_size = GetPacketSizeFromCode(size_code);
    if (total_size == 0 || total_size > buffer_size) {
        return 0;  // Invalid size or buffer too small
    }

    // Reject payloads that exceed the largest size class's capacity
    // (2042 = 2048 - header(4) - crc(2)). GetOptimalSizeCode saturates to
    // PKT_SIZE_2048 for anything larger, so without this check a payload of
    // 2043-2048 bytes would memcpy past the CRC region and the zero-pad
    // memset length below would underflow size_t into a wild multi-GB
    // memset (review H4).
    if (payload_size > total_size - 6) {
        return 0;
    }

    buffer[0] = PKT_MAKE_FLAGS_SIZE(size_code, flags);
    buffer[1] = msg_type;
    // The masks make these provably in range; the casts keep -Wconversion
    // quiet so a genuinely lossy narrowing stands out here later.
    buffer[2] = static_cast<uint8_t>(sequence_number & 0xFF);         // seq low byte
    buffer[3] = static_cast<uint8_t>((sequence_number >> 8) & 0xFF);  // seq high byte

    // Copy payload. The guard is not redundant with payload_size: a
    // zero-length payload is a legitimate frame (heartbeats, ACKs) and
    // callers pass payload == nullptr for it, but memcpy declares both
    // pointers non-null, so memcpy(dst, nullptr, 0) is undefined even though
    // it copies nothing. Caught by UBSan on the first `make test-asan` run.
    // It matters beyond pedantry: the compiler may infer from the nonnull
    // attribute that `payload` cannot be null and delete later null checks.
    if (payload && payload_size > 0) {
        memcpy(buffer + 4, payload, payload_size);
    }

    // Zero the padding up to the size class. Receivers rely on this: fields
    // appended to a message parse as zero on a peer still sending the older,
    // shorter layout (see DiagPushMessage's heap fields).
    memset(buffer + 4 + payload_size, 0, total_size - 4 - payload_size - 2);

    // Calculate CRC over entire packet except CRC field
    uint16_t crc = CalculateWaveXCrc(buffer, total_size - 2);
    buffer[total_size - 2] = static_cast<uint8_t>(crc & 0xFF);
    buffer[total_size - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    return total_size;
}

bool ProtocolHandler::ParseWaveXPacket(const uint8_t* buffer,
                                       size_t buffer_size,
                                       uint8_t& msg_type,
                                       void* payload,
                                       size_t& payload_size,
                                       uint16_t& sequence_number,
                                       uint8_t& flags) {
    // Validate minimum packet size
    if (buffer_size < 6)
        return false;  // header(4) + crc(2)

    // Extract header information
    uint8_t size_code = buffer[0] & PKT_SIZE_MASK;
    flags = PKT_GET_FLAGS(buffer[0]);
    msg_type = buffer[1];
    sequence_number = buffer[2] | (buffer[3] << 8);  // 16-bit sequence number

    // Get actual packet size
    size_t total_size = GetPacketSizeFromCode(size_code);
    if (total_size != buffer_size)
        return false;

    // Validate CRC
    if (!ValidateWaveXPacket(buffer, buffer_size))
        return false;

    // Extract payload. `payload_size` is in/out: on entry it is the caller's
    // destination buffer capacity; copy no more than that, even though the
    // packet's payload region (zero-padded up to the size class) may be
    // larger. Without this clamp, a caller sizing its buffer to a specific
    // message struct (e.g. ErrorMessage, 50 bytes) can be overrun by a larger
    // padded region (e.g. 58 bytes for a 64-byte packet) - this was a real,
    // previously-latent stack/heap overflow caught by round-trip tests.
    size_t available_payload = total_size - 6;  // header(4) + crc(2)
    size_t to_copy = payload_size < available_payload ? payload_size : available_payload;
    if (payload && to_copy > 0) {
        memcpy(payload, buffer + 4, to_copy);
    }
    payload_size = to_copy;

    return true;
}

// Legacy entry point, kept for compatibility.
bool ProtocolHandler::ValidatePacketCrc(const uint8_t* packet_data, size_t packet_size) {
    // A frame is a 4-byte header plus the 2-byte CRC; anything shorter cannot
    // carry a CRC to check, and indexing [packet_size - 2] would read out of
    // bounds. Matches the minimum ValidateWaveXPacket enforces.
    if (!packet_data || packet_size < 4 + sizeof(uint16_t)) {
        return false;
    }
    uint16_t calculated_crc = CalculatePacketCrc(packet_data, packet_size);
    uint16_t received_crc = packet_data[packet_size - 2] | (packet_data[packet_size - 1] << 8);
    return calculated_crc == received_crc;
}

void ProtocolHandler::ZeroPadPacket(uint8_t* packet_data, size_t packet_size, size_t used_size) {
    if (used_size < packet_size) {
        memset(packet_data + used_size, 0, packet_size - used_size);
    }
}

// Shared funnel for the per-message Create*Packet wrappers below.
static size_t CreateUnifiedPacket(uint8_t* buffer,
                                  size_t buffer_size,
                                  uint8_t msg_type,
                                  const void* payload_data,
                                  size_t payload_size,
                                  uint8_t flags = 0) {
    return ProtocolHandler::CreatePacket(
        buffer, buffer_size, msg_type, payload_data, payload_size, flags);
}

size_t ProtocolHandler::CreateErrorPacket(uint8_t* buffer,
                                          size_t buffer_size,
                                          const ErrorMessage& err) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_ERROR, &err, sizeof(ErrorMessage));
}

size_t ProtocolHandler::CreateSampleStatusPacket(uint8_t* buffer,
                                                 size_t buffer_size,
                                                 const SampleStatusMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_STATUS, &msg, sizeof(SampleStatusMessage));
}

size_t ProtocolHandler::CreateStorageStatusPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const StorageStatusMessage& status) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_STORAGE_STATUS, &status, sizeof(StorageStatusMessage));
}

size_t ProtocolHandler::CreateSampleMetaPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               const SampleMetadata& msg) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_SAMPLE_META, &msg, sizeof(SampleMetadata));
}

size_t ProtocolHandler::CreateSampleMetaReqPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const SampleMetaReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_META_REQ, &msg, sizeof(SampleMetaReqMessage));
}

size_t ProtocolHandler::CreateTrackBindingReqPacket(uint8_t* buffer,
                                                    size_t buffer_size,
                                                    const TrackBindingReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_TRACK_BINDING_REQ, &msg, sizeof(TrackBindingReqMessage));
}

size_t ProtocolHandler::CreateTrackBindingPacket(uint8_t* buffer,
                                                 size_t buffer_size,
                                                 const TrackBindingMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_TRACK_BINDING, &msg, sizeof(TrackBindingMessage));
}

size_t ProtocolHandler::CreateMixOpPacket(uint8_t* buffer,
                                          size_t buffer_size,
                                          const MixOpMessage& msg) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_MIX_OP, &msg, sizeof(MixOpMessage));
}

size_t ProtocolHandler::CreateMixMetersPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const MixMetersMessage& msg) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_MIX_METERS, &msg, sizeof(MixMetersMessage));
}

size_t ProtocolHandler::CreateSampleEditPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               const SampleEditMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_EDIT_SET, &msg, sizeof(SampleEditMessage));
}

size_t ProtocolHandler::CreateDiagSubscribePacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const DiagSubscribeMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_DIAG_SUBSCRIBE, &msg, sizeof(DiagSubscribeMessage));
}

size_t ProtocolHandler::CreateDiagPushPacket(uint8_t* buffer,
                                             size_t buffer_size,
                                             const DiagPushMessage& msg) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_DIAG_PUSH, &msg, sizeof(DiagPushMessage));
}

size_t ProtocolHandler::CreateSampleStopRespPacket(uint8_t* buffer,
                                                   size_t buffer_size,
                                                   const SampleStopRespMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_STOP_RESP, &msg, sizeof(SampleStopRespMessage));
}

size_t ProtocolHandler::CreateSampleStopReqPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const SampleStopReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_STOP_REQ, &msg, sizeof(SampleStopReqMessage));
}

size_t ProtocolHandler::CreateBrowseRespPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               uint32_t total_count,
                                               const FileEntryWire* entries,
                                               uint8_t n) {
    // Payload layout: [total_count u32][n u8][n * FileEntryWire]
    size_t payload_size = sizeof(uint32_t) + sizeof(uint8_t) + (size_t)n * sizeof(FileEntryWire);

    uint8_t temp_payload[2048];  // staging; MAX_PKT_SIZE-sized
    if (payload_size > sizeof(temp_payload)) {
        return 0;
    }

    memcpy(temp_payload, &total_count, sizeof(uint32_t));
    temp_payload[sizeof(uint32_t)] = n;
    if (n > 0 && entries != NULL) {
        memcpy(temp_payload + sizeof(uint32_t) + sizeof(uint8_t),
               entries,
               (size_t)n * sizeof(FileEntryWire));
    }

    return CreateUnifiedPacket(buffer, buffer_size, MSG_BROWSE_RESP, temp_payload, payload_size);
}

size_t ProtocolHandler::CreateSamplePathResponsePacket(uint8_t* buffer,
                                                       size_t buffer_size,
                                                       const SamplePathResponseMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_GET_PATH_RESP, &msg, sizeof(SamplePathResponseMessage));
}

size_t ProtocolHandler::CreateControlChangePacket(
    uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value) {
    ControlChangeMessage msg(parameter, channel, value);
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_CONTROL_CHANGE, &msg, sizeof(ControlChangeMessage));
}

size_t ProtocolHandler::CreateNoteOnPacket(
    uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel) {
    NoteMessage msg(note, velocity, channel);
    return CreateUnifiedPacket(buffer, buffer_size, MSG_NOTE_ON, &msg, sizeof(NoteMessage));
}

size_t ProtocolHandler::CreateNoteOffPacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            uint8_t note,
                                            uint8_t channel) {
    NoteMessage msg(note, 0, channel);
    return CreateUnifiedPacket(buffer, buffer_size, MSG_NOTE_OFF, &msg, sizeof(NoteMessage));
}

size_t ProtocolHandler::CreateSampleCtrlPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               const SampleCtrlMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_CTRL, &msg, sizeof(SampleCtrlMessage));
}

size_t ProtocolHandler::CreatePreviewReqPacket(uint8_t* buffer,
                                               size_t buffer_size,
                                               const PreviewReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_PREVIEW_REQ, &msg, sizeof(PreviewReqMessage));
}

size_t ProtocolHandler::CreateDataRequestPacket(uint8_t* buffer,
                                                size_t buffer_size,
                                                const DataRequestMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_DATA_REQUEST, &msg, sizeof(DataRequestMessage));
}

size_t ProtocolHandler::CreateMeterPushPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const MeterPushMessage& msg) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_METER_PUSH, &msg, sizeof(MeterPushMessage));
}

size_t ProtocolHandler::CreateSyncPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         const SyncMessage& msg) {
    return CreatePacket(buffer, buffer_size, MSG_SYNC, &msg, sizeof(SyncMessage), 0);
}

size_t ProtocolHandler::CreateHeartbeatPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const HeartbeatMessage& msg) {
    return CreatePacket(buffer, buffer_size, MSG_HEARTBEAT, &msg, sizeof(HeartbeatMessage), 0);
}

size_t ProtocolHandler::CreateAckPacket(uint8_t* buffer,
                                        size_t buffer_size,
                                        const AckMessage& ack) {
    return CreateUnifiedPacket(buffer, buffer_size, MSG_ACK, &ack, sizeof(AckMessage));
}

size_t ProtocolHandler::CreateSamplePlayIndexPacket(uint8_t* buffer,
                                                    size_t buffer_size,
                                                    const SamplePlayIndexMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_PLAY_INDEX_REQ, &msg, sizeof(SamplePlayIndexMessage));
}

size_t ProtocolHandler::CreateSampleGetPathPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const SampleGetPathMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_SAMPLE_GET_PATH_REQ, &msg, sizeof(SampleGetPathMessage));
}

// Envelope request (frontend -> backend).
size_t ProtocolHandler::CreateEnvelopeReqPacket(uint8_t* buffer,
                                                size_t buffer_size,
                                                const EnvelopeReqMessage& msg) {
    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_ENVELOPE_REQ, &msg, sizeof(EnvelopeReqMessage));
}

// One run of envelope columns (backend -> frontend). Header then
// column_count EnvelopeColumn values; the caller is responsible for having
// sized the run to fit a packet (header + count * 4 <= the largest payload).
size_t ProtocolHandler::CreateEnvelopeChunkPacket(uint8_t* buffer,
                                                  size_t buffer_size,
                                                  const EnvelopeChunkMessage& msg,
                                                  const EnvelopeColumn* columns,
                                                  size_t column_count) {
    const size_t header_size = sizeof(EnvelopeChunkMessage);
    const size_t data_size = column_count * sizeof(EnvelopeColumn);
    const size_t total_payload_size = header_size + data_size;

    uint8_t temp_payload[MAX_PKT_SIZE];
    if (total_payload_size > sizeof(temp_payload)) {
        return 0;
    }

    memcpy(temp_payload, &msg, header_size);
    if (columns && data_size > 0) {
        memcpy(temp_payload + header_size, columns, data_size);
    }

    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_ENVELOPE_CHUNK, temp_payload, total_payload_size);
}

size_t ProtocolHandler::CreateWaveChunkPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const WaveChunkMessage& msg,
                                              const void* sample_data,
                                              size_t sample_data_size) {
    // Payload layout: WaveChunkMessage header, then msg.count int16 samples.
    size_t header_size = sizeof(WaveChunkMessage);
    size_t total_payload_size = header_size + sample_data_size;

    uint8_t temp_payload[2048];  // staging; MAX_PKT_SIZE-sized
    if (total_payload_size > sizeof(temp_payload)) {
        return 0;
    }

    memcpy(temp_payload, &msg, header_size);
    if (sample_data && sample_data_size > 0) {
        memcpy(temp_payload + header_size, sample_data, sample_data_size);
    }

    return CreateUnifiedPacket(
        buffer, buffer_size, MSG_WAVE_CHUNK, temp_payload, total_payload_size);
}

// (Review H6/M10: ParseBrowseReq/ParseSamplePlayReq were deleted here.
// They parsed a browse-request wire format - path-first, u32 start_index,
// u8 max_entries - that nothing ever sent: the live format is
// [start_index u8][path][NUL], built by inter_mcu_send_browse_req and
// parsed by HandleBrowseRequestMessage, pinned by the dispatch host test.
// The wire contract must not exist in two disagreeing copies.)

// Compatibility wrapper functions for tests
bool ProtocolHandler::ValidatePacket(const uint8_t* buffer, size_t length) {
    return ValidateWaveXPacket(buffer, length);
}

MessageType ProtocolHandler::GetMessageType(const uint8_t* buffer) {
    if (!buffer)
        return MSG_ERROR;
    size_t packet_size = GetPacketSize(buffer);
    if (packet_size < 2)
        return MSG_ERROR;
    return static_cast<MessageType>(buffer[1]);
}

bool ProtocolHandler::ParseControlChange(const uint8_t* buffer, ControlChangeMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(ControlChangeMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_CONTROL_CHANGE;
}

bool ProtocolHandler::ParseNoteMessage(const uint8_t* buffer, NoteMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(NoteMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return (msg_type == MSG_NOTE_ON || msg_type == MSG_NOTE_OFF);
}

bool ProtocolHandler::ParseSampleCtrl(const uint8_t* buffer, SampleCtrlMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(SampleCtrlMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_SAMPLE_CTRL;
}

bool ProtocolHandler::ParsePreviewReq(const uint8_t* buffer, PreviewReqMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(PreviewReqMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_PREVIEW_REQ;
}

bool ProtocolHandler::ParseDataRequest(const uint8_t* buffer, DataRequestMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(DataRequestMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_DATA_REQUEST;
}

bool ProtocolHandler::ParseMessage(const uint8_t* buffer, HeartbeatMessage& msg) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = sizeof(HeartbeatMessage);
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, &msg, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == MSG_HEARTBEAT;
}

bool ProtocolHandler::ParseMessage(const uint8_t* buffer,
                                   MessageType expected_type,
                                   void* out_payload,
                                   size_t out_payload_size) {
    uint8_t msg_type;
    uint16_t seq;
    uint8_t flags;
    size_t payload_size = out_payload_size;
    size_t packet_size = GetPacketSize(buffer);

    if (packet_size == 0)
        return false;
    if (!ParseWaveXPacket(buffer, packet_size, msg_type, out_payload, payload_size, seq, flags)) {
        return false;
    }
    return msg_type == static_cast<uint8_t>(expected_type);
}

size_t ProtocolHandler::GetPacketSize(const uint8_t* buffer) {
    if (!buffer)
        return 0;
    uint8_t size_code = buffer[0] & PKT_SIZE_MASK;
    return GetPacketSizeFromCode(size_code);
}

uint8_t ProtocolHandler::CalculateChecksum(const uint8_t* data, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

}  // namespace Protocol
}  // namespace WaveX
