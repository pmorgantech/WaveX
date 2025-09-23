#ifndef WAVEX_PROTOCOL_H
#define WAVEX_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

namespace WaveX {
namespace Protocol {

// Protocol constants
static const uint8_t MAX_PAYLOAD_SIZE = 220; // allow multi-entry directory responses
static const uint32_t PROTOCOL_VERSION = 1;

// Flexible Packet Type Space Allocation

// Command Packet Types (0x01-0x7F) - 127 available
#define PKT_TYPE_CMD32        0x01  // Command packet (32 bytes)
#define PKT_TYPE_CMD64        0x02  // Command packet (64 bytes)
#define PKT_TYPE_CMD128       0x03  // Command packet (128 bytes)
#define PKT_TYPE_CMD256       0x04  // Command packet (256 bytes)
#define PKT_TYPE_CMD512       0x05  // Command packet (512 bytes)
#define PKT_TYPE_CMD1024      0x06  // Command packet (1024 bytes)
#define PKT_TYPE_CMD2048      0x07  // Command packet (2048 bytes)
#define PKT_TYPE_CMD4096      0x08  // Command packet (4096 bytes)
// ... reserve 0x09-0x7F for future command packet types

// Data Packet Types (0x80-0xFE) - 127 available
#define PKT_TYPE_DATA32      0x80  // Data packet (32 bytes)
#define PKT_TYPE_DATA64      0x81  // Data packet (64 bytes)
#define PKT_TYPE_DATA128     0x82  // Data packet (128 bytes)
#define PKT_TYPE_DATA256     0x83  // Data packet (256 bytes)
#define PKT_TYPE_DATA512     0x84  // Data packet (512 bytes)
#define PKT_TYPE_DATA1024    0x85  // Data packet (1024 bytes)
#define PKT_TYPE_DATA2048    0x86  // Data packet (2048 bytes)
#define PKT_TYPE_DATA4096    0x87  // Data packet (4096 bytes)
// ... reserve 0x88-0xFE for future data packet types

// Reserved/Control Types
#define PKT_TYPE_NULL        0x00  // Empty/null packet
#define PKT_TYPE_ERROR_VAL   0xFF  // Error/invalid packet

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
    MSG_ACK = 0x35,  // Acknowledgment message
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

// Acknowledgment message
struct AckMessage {
    uint16_t serial_id;  // Serial ID of the message being acknowledged
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

// Legacy Packet structure removed - using flexible packet system only

// Flexible Packet Structures (CRC at end for optimal performance)

// Command packet structures (CRC at end)
struct SpiCommandPacket32 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD32)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-20)
    uint8_t payload[20]; // Command payload (20 bytes)
    uint8_t padding[5];  // Alignment padding (5 bytes to make total 32)
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket64 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD64)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-52)
    uint8_t payload[52];  // Command payload (52 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket128 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD128)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-116)
    uint8_t payload[116]; // Command payload (116 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket256 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD256)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-244)
    uint8_t payload[244]; // Command payload (244 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket512 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD512)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-500)
    uint8_t payload[500]; // Command payload (500 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket1024 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD1024)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-1012)
    uint8_t payload[1012]; // Command payload (1012 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket2048 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD2048)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-2036)
    uint8_t payload[2036]; // Command payload (2036 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiCommandPacket4096 {
    uint8_t type;         // Packet type (PKT_TYPE_CMD4096)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-4084)
    uint8_t payload[4084]; // Command payload (4084 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

// Data packet structures (CRC at end)
struct SpiDataPacket32 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA32)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-20)
    uint8_t payload[20];  // Data payload (20 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket64 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA64)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-52)
    uint8_t payload[52];  // Data payload (52 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket128 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA128)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-116)
    uint8_t payload[116]; // Data payload (116 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket256 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA256)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-244)
    uint8_t payload[244]; // Data payload (244 bytes)
    uint8_t padding[5];   // Alignment padding (5 bytes to make total 256)
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket512 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA512)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-500)
    uint8_t payload[500]; // Data payload (500 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket1024 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA1024)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-1012)
    uint8_t payload[1012]; // Data payload (1012 bytes)
    uint8_t padding[5];   // Alignment padding (5 bytes to make total 1024)
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket2048 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA2048)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-2036)
    uint8_t payload[2036]; // Data payload (2036 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

struct SpiDataPacket4096 {
    uint8_t type;         // Packet type (PKT_TYPE_DATA4096)
    uint8_t flags;        // ACK/REQ/NACK/MORE flags
    uint16_t seq;         // Sequence number (2 bytes)
    uint8_t len;          // Payload length (0-4084)
    uint8_t payload[4084]; // Data payload (4084 bytes)
    uint8_t padding[6];   // Alignment padding
    uint16_t crc;         // CRC16-CCITT over header + payload (at end)
} __attribute__((packed));

// Legacy packet structure for compatibility
struct LegacyPacket {
    uint8_t sync;           // Always SYNC_BYTE
    uint8_t type;           // MessageType
    uint8_t length;         // Payload length
    uint8_t checksum;       // Simple XOR checksum
    uint8_t payload[MAX_PAYLOAD_SIZE];
} __attribute__((packed));

// Flexible packet size constants
static const size_t PKT_SIZE_32 = 32;
static const size_t PKT_SIZE_64 = 64;
static const size_t PKT_SIZE_128 = 128;
static const size_t PKT_SIZE_256 = 256;
static const size_t PKT_SIZE_512 = 512;
static const size_t PKT_SIZE_1024 = 1024;
static const size_t PKT_SIZE_2048 = 2048;
static const size_t PKT_SIZE_4096 = 4096;

// Maximum packet size support
static const size_t MAX_PKT_SIZE = PKT_SIZE_4096;

// Sanity checks for packet sizes
static_assert(sizeof(SpiCommandPacket32) == 32, "Command packet 32 size drift");
static_assert(sizeof(SpiDataPacket256) == 256, "Data packet 256 size drift");
static_assert(sizeof(SpiDataPacket1024) == 1024, "Data packet 1024 size drift");

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
    static size_t CreateBrowseRespPacket(uint8_t* buffer, size_t buffer_size,
                                         uint32_t total_count,
                                         const FileEntryWire* entries,
                                         uint8_t n);
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
    
    // Flexible packet system functions
    static size_t GetPacketSizeFromType(uint8_t packet_type);
    static bool IsCommandPacketType(uint8_t packet_type);
    static bool IsDataPacketType(uint8_t packet_type);
    static uint8_t GetOptimalPacketType(size_t payload_size, bool is_command);
    static uint16_t CalculateSpiCrc(const uint8_t* data, size_t length);
    static uint16_t CalculatePacketCrc(const uint8_t* packet_data, size_t packet_size);
    static bool ValidatePacketCrc(const uint8_t* packet_data, size_t packet_size);
    static void ZeroPadPacket(uint8_t* packet_data, size_t packet_size, size_t used_size);
    
    // Utility functions
    static uint8_t CalculateChecksum(const uint8_t* data, size_t length);
    static size_t GetPacketSize(const uint8_t* buffer);
};

} // namespace Protocol
} // namespace WaveX

#endif // WAVEX_PROTOCOL_H 