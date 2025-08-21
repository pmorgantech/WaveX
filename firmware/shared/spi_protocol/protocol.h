#ifndef WAVEX_PROTOCOL_H
#define WAVEX_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

namespace WaveX {
namespace Protocol {

// Protocol constants
static const uint8_t SYNC_BYTE = 0xAA;
static const uint8_t MAX_PAYLOAD_SIZE = 220; // allow multi-entry directory responses
static const uint32_t PROTOCOL_VERSION = 1;

// Message types
enum MessageType : uint8_t {
    MSG_SYNC = 0x00,
    MSG_CONTROL_CHANGE = 0x01,
    MSG_NOTE_ON = 0x02,
    MSG_NOTE_OFF = 0x03,
    MSG_SAMPLE_LOAD = 0x04,
    MSG_SAMPLE_DATA = 0x05,
    MSG_PARAMETER_UPDATE = 0x06,
    MSG_STATUS_REQUEST = 0x07,
    MSG_STATUS_RESPONSE = 0x08,
    // Phase I new message types
    MSG_SAMPLE_CTRL = 0x09,
    MSG_PREVIEW_REQ = 0x0A,
    MSG_DATA_REQUEST = 0x0B, // ESP32 requests queued data from Daisy
    MSG_METER_PUSH  = 0x10, // backend -> frontend
    MSG_WAVE_CHUNK  = 0x11, // backend -> frontend
    MSG_HEARTBEAT   = 0x12, // periodic health beacon
    // File browse and sample playback control (MVP)
    MSG_BROWSE_REQ   = 0x30,
    MSG_BROWSE_RESP  = 0x31,
    MSG_SAMPLE_PLAY_REQ = 0x32,
    MSG_SAMPLE_STOP_REQ = 0x33,
    MSG_SAMPLE_STATUS   = 0x34,
    MSG_ERROR = 0xFF
};

// Control change parameters
enum ControlParameter : uint8_t {
    PARAM_VOLUME = 0x01,
    PARAM_FILTER_CUTOFF = 0x02,
    PARAM_FILTER_RESONANCE = 0x03,
    PARAM_ENVELOPE_ATTACK = 0x04,
    PARAM_ENVELOPE_DECAY = 0x05,
    PARAM_ENVELOPE_SUSTAIN = 0x06,
    PARAM_ENVELOPE_RELEASE = 0x07,
    PARAM_LFO_RATE = 0x08,
    PARAM_LFO_DEPTH = 0x09,
    PARAM_MODULATION_MATRIX = 0x0A
};

// Packet header structure
struct PacketHeader {
    uint8_t sync;           // Always SYNC_BYTE
    uint8_t type;           // MessageType
    uint8_t length;         // Payload length
    uint8_t checksum;       // Simple XOR checksum
} __attribute__((packed));

// Control change message
struct ControlChangeMessage {
    uint8_t parameter;      // ControlParameter
    uint8_t channel;        // 0-15
    uint16_t value;         // 0-65535
} __attribute__((packed));

// Note message
struct NoteMessage {
    uint8_t note;           // MIDI note number
    uint8_t velocity;       // 0-127
    uint8_t channel;        // 0-15
    uint8_t reserved;       // Reserved for future use
} __attribute__((packed));

// Sample load message
struct SampleLoadMessage {
    uint16_t sample_id;     // Unique sample identifier
    uint32_t sample_size;   // Size in bytes
    uint16_t sample_rate;   // Sample rate in Hz
    uint8_t channels;       // Number of channels (1 or 2)
    uint8_t bit_depth;      // Bit depth (16 or 24)
} __attribute__((packed));

// Sample control message
enum SampleCtrlCmd : uint8_t { SAMPLE_REC_START=1, SAMPLE_REC_STOP=2, SAMPLE_PLAY_START=3, SAMPLE_PLAY_STOP=4 };
struct SampleCtrlMessage {
    uint8_t slot;           // 0 for now
    uint8_t cmd;            // SampleCtrlCmd
    float rate;             // playback rate (1.0 = normal)
} __attribute__((packed));

// Preview request message
struct PreviewReqMessage {
    uint8_t slot;           // 0 for now
    uint32_t start;         // sample index start
    uint32_t end;           // sample index end (exclusive)
    uint16_t decim;         // decimation factor
} __attribute__((packed));

// Data request message (ESP32 → Daisy: request queued data)
struct DataRequestMessage {
    uint8_t request_type;   // 0 = any data, 1 = meter data, 2 = wave data
    uint8_t reserved[3];    // Reserved for future use
} __attribute__((packed));

// Meter push (backend->frontend)
struct MeterPushMessage {
    float rms;
    float peak;
} __attribute__((packed));

// Wave chunk (backend->frontend)
struct WaveChunkMessage {
    uint32_t offset;        // index in preview stream
    uint16_t count;         // number of int16 samples following
    // payload follows (count * int16)
} __attribute__((packed));

// File browse protocol (variable-size payloads)
static const size_t FILE_NAME_MAX = 48;
static const size_t BROWSE_PATH_MAX = 96;

struct FileEntryWire {
    uint8_t  is_dir;
    uint32_t size_bytes;
    char     name[FILE_NAME_MAX];
} __attribute__((packed));

struct BrowseRespHeader {
    uint32_t total_count;
    uint8_t  n;
} __attribute__((packed));

// Sample status
struct SampleStatusMessage {
    uint8_t  state;          // 0=stopped,1=playing,2=ended
    uint32_t sample_rate;
    uint8_t  channels;
    uint32_t frames_played;
} __attribute__((packed));

// Error message (short)
struct ErrorMessage {
    uint16_t code;
    char     msg[48];
} __attribute__((packed));

// Heartbeat (bidirectional)
struct HeartbeatMessage {
    uint32_t uptime_ms;
    uint32_t rx_total;
    uint32_t loop_counter;
} __attribute__((packed));

// Generic packet structure
struct Packet {
    PacketHeader header;
    uint8_t payload[MAX_PAYLOAD_SIZE];
} __attribute__((packed));

// Protocol functions
class ProtocolHandler {
public:
    // Packet creation
    static size_t CreateControlChangePacket(uint8_t* buffer, size_t buffer_size,
                                          uint8_t parameter, uint8_t channel, uint16_t value);
    
    static size_t CreateNoteOnPacket(uint8_t* buffer, size_t buffer_size,
                                   uint8_t note, uint8_t velocity, uint8_t channel);
    
    static size_t CreateNoteOffPacket(uint8_t* buffer, size_t buffer_size,
                                    uint8_t note, uint8_t channel);

    // Phase I: additional packet creators
    static size_t CreateSampleCtrlPacket(uint8_t* buffer, size_t buffer_size,
                                       const SampleCtrlMessage& msg);

    static size_t CreatePreviewReqPacket(uint8_t* buffer, size_t buffer_size,
                                       const PreviewReqMessage& msg);

    static size_t CreateDataRequestPacket(uint8_t* buffer, size_t buffer_size,
                                        const DataRequestMessage& msg);
    
    static size_t CreateMeterPushPacket(uint8_t* buffer, size_t buffer_size,
                                      const MeterPushMessage& msg);
    
    static size_t CreateWaveChunkPacket(uint8_t* buffer, size_t buffer_size,
                                      const WaveChunkMessage& msg, const void* sample_data, size_t sample_data_size);
    
    static size_t CreateGenericPacket(uint8_t* buffer, size_t buffer_size,
                                    uint8_t type, const void* payload, size_t payload_size);
    
    static size_t CreateHeartbeatPacket(uint8_t* buffer, size_t buffer_size,
                                      const HeartbeatMessage& msg);

    // Browse and sample control helpers
    static bool   ParseBrowseReq(const uint8_t* buffer, char* path_out, size_t path_max,
                                 uint32_t& start_index, uint8_t& max_entries);
    static size_t CreateBrowseRespPacket(uint8_t* buffer, size_t buffer_size,
                                         uint32_t total_count,
                                         const FileEntryWire* entries,
                                         uint8_t n);
    static bool   ParseSamplePlayReq(const uint8_t* buffer, char* path_out, size_t path_max);
    static size_t CreateSampleStatusPacket(uint8_t* buffer, size_t buffer_size,
                                           const SampleStatusMessage& msg);
    static size_t CreateErrorPacket(uint8_t* buffer, size_t buffer_size,
                                    const ErrorMessage& err);
    
    // Packet parsing
    static bool ValidatePacket(const uint8_t* buffer, size_t length);
    static MessageType GetMessageType(const uint8_t* buffer);
    static bool ParseControlChange(const uint8_t* buffer, ControlChangeMessage& msg);
    static bool ParseNoteMessage(const uint8_t* buffer, NoteMessage& msg);
    static bool ParseSampleCtrl(const uint8_t* buffer, SampleCtrlMessage& msg);
    static bool ParsePreviewReq(const uint8_t* buffer, PreviewReqMessage& msg);
    static bool ParseDataRequest(const uint8_t* buffer, DataRequestMessage& msg);
    static bool ParseMessage(const uint8_t* buffer, HeartbeatMessage& msg);
    // Generic payload parser for fixed-size messages
    static bool ParseMessage(const uint8_t* buffer, MessageType expected_type, void* out_payload, size_t out_payload_size);
    
    // Utility functions
    static uint8_t CalculateChecksum(const uint8_t* data, size_t length);
    static size_t GetPacketSize(const uint8_t* buffer);
};

} // namespace Protocol
} // namespace WaveX

#endif // WAVEX_PROTOCOL_H 