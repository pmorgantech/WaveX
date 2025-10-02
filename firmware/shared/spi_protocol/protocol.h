#ifndef WAVEX_PROTOCOL_H
#define WAVEX_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

namespace WaveX {
namespace Protocol {

// Protocol constants
static const uint8_t MAX_PAYLOAD_SIZE = 220; // allow multi-entry directory responses
static const uint32_t PROTOCOL_VERSION = 1;

// Simplified Packet Format - Clean and Efficient

// Single Unified Packet Format - no command vs data distinction
struct WaveXPacket {
    uint8_t  flags_size;   // 4 LSB = size code, 4 MSB = flags
    uint8_t  msg_type;     // Message type (MSG_SYNC, MSG_HEARTBEAT, etc.)
    uint16_t seq;          // 16-bit sequence number (0-65535)
    uint8_t  payload[0];   // Variable length payload (structured message data)
    uint16_t crc;          // CRC16-CCITT over entire packet except CRC (at end)
} __attribute__((packed));

// Size encoding (4 LSB of flags_size)
#define PKT_SIZE_32      0x00  // 32 bytes total
#define PKT_SIZE_64      0x01  // 64 bytes total
#define PKT_SIZE_128     0x02  // 128 bytes total
#define PKT_SIZE_256     0x03  // 256 bytes total
#define PKT_SIZE_512     0x04  // 512 bytes total (future)
#define PKT_SIZE_1024    0x05  // 1024 bytes total
#define PKT_SIZE_2048    0x06  // 2048 bytes total

// Flags (4 MSB of flags_size)
#define PKT_FLAG_ACK     0x80  // This is an acknowledgment packet
#define PKT_FLAG_NACK    0x40  // This packet is corrupted/needs resent
#define PKT_FLAG_RES1    0x20  // Reserved for future use
#define PKT_FLAG_RES2    0x10  // Reserved for future use

// Masks
#define PKT_SIZE_MASK    0x0F  // 4 LSB for size encoding
#define PKT_FLAG_MASK    0xF0  // 4 MSB for flags

// Helper macros
#define PKT_GET_SIZE(flags_size) ((flags_size & PKT_SIZE_MASK) == 0 ? 32 : \
                                 (flags_size & PKT_SIZE_MASK) == 1 ? 64 : \
                                 (flags_size & PKT_SIZE_MASK) == 2 ? 128 : \
                                 (flags_size & PKT_SIZE_MASK) == 3 ? 256 : \
                                 (flags_size & PKT_SIZE_MASK) == 4 ? 512 : \
                                 (flags_size & PKT_SIZE_MASK) == 5 ? 1024 : 2048)

#define PKT_GET_FLAGS(flags_size) (flags_size & PKT_FLAG_MASK)
#define PKT_SET_FLAGS(flags_size, flags) ((flags_size & PKT_SIZE_MASK) | (flags & PKT_FLAG_MASK))
#define PKT_MAKE_FLAGS_SIZE(size_code, flags) ((flags & PKT_FLAG_MASK) | (size_code & PKT_SIZE_MASK))

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
    MSG_ACK = 0x13,  // Acknowledgment message
    // File browse and sample playback control (MVP)
    MSG_BROWSE_REQ   = 0x30,
    MSG_BROWSE_RESP  = 0x31,
    MSG_SAMPLE_PLAY_REQ = 0x32,
    MSG_SAMPLE_STOP_REQ = 0x33,
    MSG_SAMPLE_STATUS   = 0x34,
    MSG_SAMPLE_STOP_RESP = 0x35,
    // Index-based file selection (new)
    MSG_SAMPLE_PLAY_INDEX_REQ = 0x36,  // Play sample by index
    MSG_SAMPLE_GET_PATH_REQ = 0x37,    // Get full path for index
    MSG_SAMPLE_GET_PATH_RESP = 0x38,   // Full path response
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

// Meter push (backend->frontend) - stereo version
struct MeterPushMessage {
    uint16_t rms_left;   // Left channel RMS (0-32767)
    uint16_t rms_right;  // Right channel RMS (0-32767)
    uint16_t peak_left;   // Left channel peak (0-32767)
    uint16_t peak_right;  // Right channel peak (0-32767)
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

// Sample stop request (now includes slot)
struct SampleStopReqMessage {
    uint8_t slot;            // Slot of currently playing sample (0 for now)
    uint8_t reserved[3];     // Reserved for alignment/future
} __attribute__((packed));

// Sample stop response
struct SampleStopRespMessage {
    uint8_t  success;        // 1=successfully stopped, 0=failed
    uint8_t  reserved[3];    // Reserved for future use
} __attribute__((packed));

// Error message (short)
struct ErrorMessage {
    uint16_t code;
    char     msg[48];
} __attribute__((packed));

// Acknowledgment message
struct AckMessage {
    uint16_t serial_id;  // Serial ID of the message being acknowledged
} __attribute__((packed));

// Sync message (simple keepalive)
struct SyncMessage {
    uint32_t timestamp_ms;  // Timestamp for sync
    uint8_t reserved[4];    // Reserved for future use
} __attribute__((packed));

// Heartbeat (bidirectional) - extended with CPU usage
struct HeartbeatMessage {
    uint32_t uptime_ms;
    uint32_t rx_total;
    uint32_t loop_counter;
    uint16_t cpu_usage_percent;  // CPU usage as percentage * 10 (e.g., 25.6% = 256)
} __attribute__((packed));

// Index-based file selection messages
struct SamplePlayIndexMessage {
    uint32_t index;         // File index in current directory
} __attribute__((packed));

struct SampleGetPathMessage {
    uint32_t index;         // File index in current directory
} __attribute__((packed));

struct SamplePathResponseMessage {
    uint32_t index;         // File index that was requested
    char     path[200];     // Full file path (null-terminated)
} __attribute__((packed));

// Legacy packet structures completely removed - using new simplified format only

// Maximum packet size support
static const size_t MAX_PKT_SIZE = 2048;

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
    
    static size_t CreateSamplePathResponsePacket(uint8_t* buffer, size_t buffer_size,
                                               const SamplePathResponseMessage& msg);
    
    // Additional flexible packet system functions
    static size_t CreateErrorPacket(uint8_t* buffer, size_t buffer_size, const ErrorMessage& err);
    static size_t CreateSampleStatusPacket(uint8_t* buffer, size_t buffer_size, const SampleStatusMessage& msg);
    static size_t CreateSampleStopRespPacket(uint8_t* buffer, size_t buffer_size, const SampleStopRespMessage& msg);
    static size_t CreateSampleStopReqPacket(uint8_t* buffer, size_t buffer_size, const SampleStopReqMessage& msg);
    static size_t CreateBrowseRespPacket(uint8_t* buffer, size_t buffer_size,
                                         uint32_t total_count,
                                         const FileEntryWire* entries,
                                         uint8_t n);
    static size_t CreateSyncPacket(uint8_t* buffer, size_t buffer_size, const SyncMessage& msg);
    static size_t CreateHeartbeatPacket(uint8_t* buffer, size_t buffer_size, const HeartbeatMessage& msg);
    static size_t CreateAckPacket(uint8_t* buffer, size_t buffer_size, const AckMessage& ack);
    static size_t CreateSamplePlayIndexPacket(uint8_t* buffer, size_t buffer_size, const SamplePlayIndexMessage& msg);
    static size_t CreateSampleGetPathPacket(uint8_t* buffer, size_t buffer_size, const SampleGetPathMessage& msg);
    
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
    
    // Additional parsing functions
    static bool ParseBrowseReq(const uint8_t* buffer, char* path_out, size_t path_max,
                              uint32_t& start_index, uint8_t& max_entries);
    static bool ParseSamplePlayReq(const uint8_t* buffer, char* path_out, size_t path_max);
    
    // New simplified packet system functions
    static size_t GetPacketSizeFromCode(uint8_t size_code);
    static uint8_t GetOptimalSizeCode(size_t payload_size);
    // Core packet creation with automatic sequence number management
    static size_t CreatePacket(uint8_t* buffer, size_t buffer_size,
                              uint8_t msg_type, const void* payload, size_t payload_size,
                              uint8_t flags = 0);
    
    static size_t CreateWaveXPacket(uint8_t* buffer, size_t buffer_size,
                                   uint8_t msg_type, const void* payload, size_t payload_size,
                                   uint16_t sequence_number = 0, uint8_t flags = 0);
    static bool ParseWaveXPacket(const uint8_t* buffer, size_t buffer_size,
                                uint8_t& msg_type, void* payload, size_t& payload_size,
                                uint16_t& sequence_number, uint8_t& flags);
    static uint16_t CalculateWaveXCrc(const uint8_t* data, size_t length);
    static uint16_t CalculateSpiCrc(const uint8_t* data, size_t length);  // Legacy compatibility
    static uint16_t CalculatePacketCrc(const uint8_t* packet_data, size_t packet_size);  // Legacy compatibility
    static bool ValidateWaveXPacket(const uint8_t* buffer, size_t buffer_size);
    static bool ValidatePacketCrc(const uint8_t* packet_data, size_t packet_size);  // Legacy compatibility
    static void ZeroPadPacket(uint8_t* packet_data, size_t packet_size, size_t used_size);  // Legacy compatibility
    
    // Utility functions
    static uint8_t CalculateChecksum(const uint8_t* data, size_t length);
    static size_t GetPacketSize(const uint8_t* buffer);
};

} // namespace Protocol
} // namespace WaveX

#endif // WAVEX_PROTOCOL_H