#ifndef WAVEX_PROTOCOL_H
#define WAVEX_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace WaveX {
namespace Protocol {

// Shared limits
#define WAVEX_SAMPLE_STATUS_MAX_ENTRIES 8

// Protocol constants
static const uint32_t PROTOCOL_VERSION = 1;

// Wire layout (review M10: a packed `WaveXPacket` struct used to "document"
// this but placed `crc` at offset 4 while the wire puts it at the packet
// end - misleading and unused, so it was deleted):
//   [0]      flags_size (4 LSB = size code, 4 MSB = flags)
//   [1]      msg_type
//   [2..3]   16-bit sequence number, little-endian (0 is reserved)
//   [4..]    payload, zero-padded to the size class
//   [N-2..]  CRC16-CCITT over bytes [0, N-2), little-endian
// Total size N is one of the PKT_SIZE_* classes (32..2048 bytes).

// Size encoding (4 LSB of flags_size)
#define PKT_SIZE_32 0x00    // 32 bytes total
#define PKT_SIZE_64 0x01    // 64 bytes total
#define PKT_SIZE_128 0x02   // 128 bytes total
#define PKT_SIZE_256 0x03   // 256 bytes total
#define PKT_SIZE_512 0x04   // 512 bytes total (future)
#define PKT_SIZE_1024 0x05  // 1024 bytes total
#define PKT_SIZE_2048 0x06  // 2048 bytes total

// Flags (4 MSB of flags_size)
#define PKT_FLAG_ACK 0x80   // This is an acknowledgment packet
#define PKT_FLAG_NACK 0x40  // This packet is corrupted/needs resent
#define PKT_FLAG_RES1 0x20  // Reserved for future use
#define PKT_FLAG_RES2 0x10  // Reserved for future use

// Masks
#define PKT_SIZE_MASK 0x0F  // 4 LSB for size encoding
#define PKT_FLAG_MASK 0xF0  // 4 MSB for flags

// Helper macros
#define PKT_GET_SIZE(flags_size)                \
    ((flags_size & PKT_SIZE_MASK) == 0   ? 32   \
     : (flags_size & PKT_SIZE_MASK) == 1 ? 64   \
     : (flags_size & PKT_SIZE_MASK) == 2 ? 128  \
     : (flags_size & PKT_SIZE_MASK) == 3 ? 256  \
     : (flags_size & PKT_SIZE_MASK) == 4 ? 512  \
     : (flags_size & PKT_SIZE_MASK) == 5 ? 1024 \
                                         : 2048)

#define PKT_GET_FLAGS(flags_size) (flags_size & PKT_FLAG_MASK)
#define PKT_SET_FLAGS(flags_size, flags) ((flags_size & PKT_SIZE_MASK) | (flags & PKT_FLAG_MASK))
#define PKT_MAKE_FLAGS_SIZE(size_code, flags) \
    ((flags & PKT_FLAG_MASK) | (size_code & PKT_SIZE_MASK))

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
    MSG_DATA_REQUEST = 0x0B,  // ESP32 requests queued data from Daisy
    MSG_METER_PUSH = 0x10,    // backend -> frontend
    MSG_WAVE_CHUNK = 0x11,    // backend -> frontend
    MSG_HEARTBEAT = 0x12,     // periodic health beacon
    MSG_ACK = 0x13,           // Acknowledgment message
    // File browse and sample playback control (MVP)
    MSG_BROWSE_REQ = 0x30,
    MSG_BROWSE_RESP = 0x31,
    MSG_SAMPLE_PLAY_REQ = 0x32,
    MSG_SAMPLE_STOP_REQ = 0x33,
    MSG_SAMPLE_STATUS = 0x34,
    MSG_SAMPLE_STOP_RESP = 0x35,
    // Index-based file selection (new)
    MSG_SAMPLE_PLAY_INDEX_REQ = 0x36,  // Play sample by index
    MSG_SAMPLE_GET_PATH_REQ = 0x37,    // Get full path for index
    MSG_SAMPLE_GET_PATH_RESP = 0x38,   // Full path response
    MSG_STORAGE_STATUS = 0x39,         // Daisy -> ESP32: SD mounted/unmounted (unsolicited)
    // Diagnostics telemetry (docs/ui-diagnostics-spec.md). Subscription-gated:
    // the push flows only while the diagnostics page is open, so it costs
    // nothing the rest of the time.
    MSG_DIAG_SUBSCRIBE = 0x3A,   // ESP32 -> Daisy: start/stop the push
    MSG_DIAG_PUSH = 0x3B,        // Daisy -> ESP32: one interval of telemetry
    MSG_SAMPLE_EDIT_SET = 0x3C,  // ESP32 -> Daisy: playback region, loop, gain (command)
    MSG_SAMPLE_META = 0x3D,      // Daisy -> ESP32: authoritative per-sample record (state)
    MSG_SAMPLE_META_REQ = 0x3E,  // ESP32 -> Daisy: resend metadata (all, or one id)
    // CV calibration (Stage A analog path - analog-voice-board.md §3)
    MSG_CV_CAL_SET = 0x40,   // ESP32 -> Daisy: apply (and optionally persist) one group's cal
    MSG_CV_CAL_GET = 0x41,   // ESP32 -> Daisy: request one group's cal
    MSG_CV_CAL_RESP = 0x42,  // Daisy -> ESP32: one group's cal (reply to SET and GET)
    MSG_CV_TEST = 0x43,      // ESP32 -> Daisy: override CVs with fixed values (cal procedure)
    // Sequencer / transport / MIDI clock (Phase 2; docs/features/sequencer.md,
    // midi-sync-tempo-follower.md, melodic-sequencing.md). ID block reserved in
    // docs/features/feature-expansion-ideas.md - do not assign outside it.
    MSG_SEQ_TRANSPORT = 0x50,   // E->D: play/stop/continue, tempo, clock source, input mode
    MSG_SEQ_PATTERN_OP = 0x51,  // E->D: small idempotent pattern edits (step/track/pattern)
    MSG_SEQ_PATTERN_SYNC =
        0x52,                 // both: bulk pattern read/write (reserved; struct not yet defined)
    MSG_SEQ_PLAYHEAD = 0x53,  // D->E: playhead/step/sync feedback (coalesced)
    MSG_MIDI_CLOCK_EVENT = 0x55,  // E->D: forwarded MIDI real-time clock/transport byte
    MSG_MIDI_CC = 0x56,           // E->D: forwarded MIDI control change
    MSG_SEQ_CLOCK_OUT = 0x57,     // D->E: MIDI clock/transport for the ESP32 to serialize outbound
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

// Common string limits (used by multiple messages)
static const size_t FILE_NAME_MAX = 48;
static const size_t BROWSE_PATH_MAX = 96;

namespace detail {
// Bounded, always-null-terminated string copy for fixed-size wire char arrays.
// dest_size must be the full array size (including the byte reserved for '\0').
inline void CopyWireString(char* dest, size_t dest_size, const char* src) {
    if (!dest || dest_size == 0)
        return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i < dest_size - 1 && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// Zero a wire struct without pulling <cstring> into this header, which is
// included by both the Daisy and ESP32 builds.
inline void ZeroWire(void* dest, size_t size) {
    uint8_t* p = static_cast<uint8_t*>(dest);
    for (size_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}
}  // namespace detail

// All message structs below are wire formats sent verbatim over SPI/UART: keep
// them packed, standard-layout, and free of virtual functions. Each has a
// zero-initializing default constructor (for the "declare then Parse() into
// it" pattern) and a named-argument constructor. Having any user-declared
// constructor makes the type a non-aggregate, so `Type x = {a, b, c};` /
// designated-initializer construction no longer compiles — construct with
// `Type x(a, b, c);` instead. This is deliberate: it forces every call site to
// be updated (and re-checked) whenever a field is added, reordered, or a
// reserved slot is repurposed, instead of silently reinterpreting positional
// braces.

// Control change message
struct ControlChangeMessage {
    uint8_t parameter;  // ControlParameter
    uint8_t channel;    // 0-15
    uint16_t value;     // 0-65535

    ControlChangeMessage() : parameter(0), channel(0), value(0) {}
    ControlChangeMessage(uint8_t parameter_, uint8_t channel_, uint16_t value_)
        : parameter(parameter_), channel(channel_), value(value_) {}
} __attribute__((packed));

// Note message
struct NoteMessage {
    uint8_t note;      // MIDI note number
    uint8_t velocity;  // 0-127
    uint8_t channel;   // 0-15
    uint8_t reserved;  // Reserved for future use

    NoteMessage() : note(0), velocity(0), channel(0), reserved(0) {}
    NoteMessage(uint8_t note_, uint8_t velocity_, uint8_t channel_)
        : note(note_), velocity(velocity_), channel(channel_), reserved(0) {}
} __attribute__((packed));

// Sample load message
struct SampleLoadMessage {
    uint16_t sample_id;          // Unique sample identifier
    uint32_t sample_size;        // Size in bytes (optional hint; Daisy re-reads)
    uint16_t sample_rate;        // Sample rate in Hz (optional hint; Daisy re-reads)
    uint8_t channels;            // Number of channels (1 or 2) (optional hint)
    uint8_t bit_depth;           // Bit depth (16 or 24) (optional hint)
    char path[BROWSE_PATH_MAX];  // Absolute/normalized path on Daisy SD

    SampleLoadMessage() : sample_id(0), sample_size(0), sample_rate(0), channels(0), bit_depth(0) {
        path[0] = '\0';
    }
    SampleLoadMessage(uint16_t sample_id_,
                      uint32_t sample_size_,
                      uint16_t sample_rate_,
                      uint8_t channels_,
                      uint8_t bit_depth_,
                      const char* path_)
        : sample_id(sample_id_),
          sample_size(sample_size_),
          sample_rate(sample_rate_),
          channels(channels_),
          bit_depth(bit_depth_) {
        detail::CopyWireString(path, sizeof(path), path_);
    }
} __attribute__((packed));

// Sample control message
enum SampleCtrlCmd : uint8_t {
    SAMPLE_REC_START = 1,
    SAMPLE_REC_STOP = 2,
    SAMPLE_PLAY_START = 3,
    SAMPLE_PLAY_STOP = 4
};
struct SampleCtrlMessage {
    uint8_t slot;  // 0 for now
    uint8_t cmd;   // SampleCtrlCmd
    float rate;    // playback rate (1.0 = normal)

    SampleCtrlMessage() : slot(0), cmd(0), rate(1.0f) {}
    SampleCtrlMessage(uint8_t slot_, uint8_t cmd_, float rate_)
        : slot(slot_), cmd(cmd_), rate(rate_) {}
} __attribute__((packed));

// Preview request message
struct PreviewReqMessage {
    uint8_t slot;    // 0 for now
    uint32_t start;  // sample index start
    uint32_t end;    // sample index end (exclusive)
    uint16_t decim;  // decimation factor

    PreviewReqMessage() : slot(0), start(0), end(0), decim(1) {}
    PreviewReqMessage(uint8_t slot_, uint32_t start_, uint32_t end_, uint16_t decim_)
        : slot(slot_), start(start_), end(end_), decim(decim_) {}
} __attribute__((packed));

// Data request message (ESP32 → Daisy: request queued data)
struct DataRequestMessage {
    uint8_t request_type;  // 0 = any data, 1 = meter data, 2 = wave data
    uint8_t reserved[3];   // Reserved for future use

    DataRequestMessage() : request_type(0), reserved{0, 0, 0} {}
    explicit DataRequestMessage(uint8_t request_type_)
        : request_type(request_type_), reserved{0, 0, 0} {}
} __attribute__((packed));

// Status request/response categories
enum StatusCategory : uint8_t {
    STATUS_CATEGORY_GENERAL = 0,
    STATUS_CATEGORY_SAMPLE_MEM = 1,
};

// Status request (ESP32 -> Daisy)
struct StatusRequestMessage {
    uint8_t category;  // StatusCategory
    uint8_t reserved[3];

    StatusRequestMessage() : category(0), reserved{0, 0, 0} {}
    explicit StatusRequestMessage(uint8_t category_) : category(category_), reserved{0, 0, 0} {}
} __attribute__((packed));

// Meter push (backend->frontend) - stereo version
struct MeterPushMessage {
    uint16_t rms_left;    // Left channel RMS (0-32767)
    uint16_t rms_right;   // Right channel RMS (0-32767)
    uint16_t peak_left;   // Left channel peak (0-32767)
    uint16_t peak_right;  // Right channel peak (0-32767)

    MeterPushMessage() : rms_left(0), rms_right(0), peak_left(0), peak_right(0) {}
    MeterPushMessage(uint16_t rms_left_,
                     uint16_t rms_right_,
                     uint16_t peak_left_,
                     uint16_t peak_right_)
        : rms_left(rms_left_),
          rms_right(rms_right_),
          peak_left(peak_left_),
          peak_right(peak_right_) {}
} __attribute__((packed));

// Wave chunk (backend->frontend)
struct WaveChunkMessage {
    uint32_t offset;  // index in preview stream
    uint16_t count;   // number of int16 samples following
    // payload follows (count * int16)

    WaveChunkMessage() : offset(0), count(0) {}
    WaveChunkMessage(uint32_t offset_, uint16_t count_) : offset(offset_), count(count_) {}
} __attribute__((packed));

struct FileEntryWire {
    uint8_t is_dir;
    uint32_t size_bytes;
    char name[FILE_NAME_MAX];
    // WAV metadata (only valid for audio files)
    uint32_t sample_rate;      // 0 if not a WAV file or unknown
    uint16_t channels;         // 0 if not a WAV file or unknown
    uint16_t bits_per_sample;  // 0 if not a WAV file or unknown
    uint32_t duration_ms;      // Duration in milliseconds (0 if unknown)

    FileEntryWire()
        : is_dir(0),
          size_bytes(0),
          sample_rate(0),
          channels(0),
          bits_per_sample(0),
          duration_ms(0) {
        name[0] = '\0';
    }
    FileEntryWire(uint8_t is_dir_,
                  uint32_t size_bytes_,
                  const char* name_,
                  uint32_t sample_rate_ = 0,
                  uint16_t channels_ = 0,
                  uint16_t bits_per_sample_ = 0,
                  uint32_t duration_ms_ = 0)
        : is_dir(is_dir_),
          size_bytes(size_bytes_),
          sample_rate(sample_rate_),
          channels(channels_),
          bits_per_sample(bits_per_sample_),
          duration_ms(duration_ms_) {
        detail::CopyWireString(name, sizeof(name), name_);
    }
} __attribute__((packed));

struct BrowseRespHeader {
    uint32_t total_count;
    uint8_t n;

    BrowseRespHeader() : total_count(0), n(0) {}
    BrowseRespHeader(uint32_t total_count_, uint8_t n_) : total_count(total_count_), n(n_) {}
} __attribute__((packed));

// Sample status (playback or load notifications)
struct SampleStatusMessage {
    uint16_t sample_id;  // logical sample identifier
    uint8_t state;       // 0=stopped,1=playing,2=ended,0x10=load complete
    uint8_t channels;
    uint32_t sample_rate;
    uint32_t frames_played;  // for load-complete: total frames loaded

    SampleStatusMessage() : sample_id(0), state(0), channels(0), sample_rate(0), frames_played(0) {}
    SampleStatusMessage(uint16_t sample_id_,
                        uint8_t state_,
                        uint8_t channels_,
                        uint32_t sample_rate_,
                        uint32_t frames_played_)
        : sample_id(sample_id_),
          state(state_),
          channels(channels_),
          sample_rate(sample_rate_),
          frames_played(frames_played_) {}
} __attribute__((packed));

// Sample stop request (now includes slot)
struct SampleStopReqMessage {
    uint8_t slot;         // Slot of currently playing sample (0 for now)
    uint8_t reserved[3];  // Reserved for alignment/future

    SampleStopReqMessage() : slot(0), reserved{0, 0, 0} {}
    explicit SampleStopReqMessage(uint8_t slot_) : slot(slot_), reserved{0, 0, 0} {}
} __attribute__((packed));

// Sample stop response
struct SampleStopRespMessage {
    uint8_t success;      // 1=successfully stopped, 0=failed
    uint8_t reserved[3];  // Reserved for future use

    SampleStopRespMessage() : success(0), reserved{0, 0, 0} {}
    explicit SampleStopRespMessage(uint8_t success_) : success(success_), reserved{0, 0, 0} {}
} __attribute__((packed));

// Storage availability, sent unsolicited by the backend when the SD card is
// mounted or lost. The frontend cannot poll for this - it has no view of the
// card slot - so without it the browser keeps showing a listing for a card
// that is gone, or an empty one for a card that has been re-inserted.
struct StorageStatusMessage {
    uint8_t mounted;      // 1 = mounted and readable, 0 = absent or unusable
    uint8_t reserved[3];  // Reserved for future use

    StorageStatusMessage() : mounted(0), reserved{0, 0, 0} {}
    explicit StorageStatusMessage(uint8_t mounted_) : mounted(mounted_), reserved{0, 0, 0} {}
} __attribute__((packed));

// How a sample's channels are rendered, wherever it is played.
enum SampleChannelMode : uint8_t {
    SAMPLE_CH_AS_RECORDED = 0,  // stereo stays stereo, mono stays mono
    SAMPLE_CH_LEFT = 1,         // left only
    SAMPLE_CH_RIGHT = 2,        // right only
    SAMPLE_CH_MONO_SUM = 3,     // (L+R)/2
};

/**
 * The authoritative per-sample record. Owned by the Daisy, pushed to the
 * frontend on every change (MSG_SAMPLE_META).
 *
 * This exists because the same facts were previously scattered and diverging:
 * geometry arrived incidentally through the browse listing, edit markers went
 * out through MSG_SAMPLE_EDIT_SET with nothing coming back, the streaming
 * audition kept its own copy of the region, VoiceManager ignored markers
 * entirely, and the preview generator silently rendered the left channel. Any
 * playback or display path that needs to know something about a sample reads
 * it from here, so those paths cannot disagree.
 *
 * `generation` increments whenever the AUDIO CONTENT changes (a destructive
 * render), not when markers move - so a waveform cache keyed on
 * (sample_id, generation) survives marker edits and is invalidated by a
 * re-render, which is exactly the desired behaviour.
 */
struct SampleMetadata {
    uint16_t sample_id;
    uint16_t generation;  // bumps on content change, not on marker edits
    uint32_t sample_rate;
    uint32_t total_frames;

    // Non-destructive playback edit. Frames, absolute, at the file's rate.
    uint32_t start_frame;
    uint32_t end_frame;  // exclusive; 0 = total_frames
    uint32_t loop_start;
    uint32_t loop_end;    // exclusive; 0 = end_frame
    int16_t gain_db_x10;  // -240..+120

    uint8_t channels;         // as stored: 1 or 2
    uint8_t bits_per_sample;  // 8 / 16 / 24
    uint8_t loop_enabled;
    uint8_t channel_mode;  // SampleChannelMode
    uint8_t flags;         // bit 0 = resident in sample RAM
    uint8_t reserved;

    char name[FILE_NAME_MAX];

    SampleMetadata()
        : sample_id(0),
          generation(0),
          sample_rate(0),
          total_frames(0),
          start_frame(0),
          end_frame(0),
          loop_start(0),
          loop_end(0),
          gain_db_x10(0),
          channels(0),
          bits_per_sample(0),
          loop_enabled(0),
          channel_mode(SAMPLE_CH_AS_RECORDED),
          flags(0),
          reserved(0) {
        name[0] = '\0';
    }

    /** Resolves the 0 sentinels against total_frames. Safe on a zeroed record. */
    void Resolve() {
        if (end_frame == 0 || end_frame > total_frames) {
            end_frame = total_frames;
        }
        if (start_frame >= end_frame) {
            start_frame = 0;
        }
        if (loop_end == 0 || loop_end > end_frame) {
            loop_end = end_frame;
        }
        if (loop_start < start_frame || loop_start >= loop_end) {
            loop_start = start_frame;
        }
    }
} __attribute__((packed));

// Request a metadata resend. sample_id 0 means "every loaded sample", which
// is how the frontend repopulates after its own restart without the backend
// having to track who has seen what.
struct SampleMetaReqMessage {
    uint16_t sample_id;
    uint8_t reserved[2];

    SampleMetaReqMessage() : sample_id(0), reserved{0, 0} {}
    explicit SampleMetaReqMessage(uint16_t sample_id_) : sample_id(sample_id_), reserved{0, 0} {}
} __attribute__((packed));

// Non-destructive playback edit (frontend -> backend).
//
// All positions are FRAMES, absolute within the file, at the file's own rate.
// The backend clamps and is the authority: it applies start <= loop_start <
// loop_end <= end and reports nothing back, so the frontend must not assume
// its values were taken verbatim.
//
// Sentinels rather than a separate "valid" flag: end_frame 0 means "to the
// end of the file" and loop_end 0 means "to end_frame". A frontend that does
// not know the file length can still send a meaningful region.
struct SampleEditMessage {
    uint8_t slot;
    uint8_t loop_enabled;
    int16_t gain_db_x10;
    // NOTE: this is the COMMAND. The backend's reply is a full SampleMetadata
    // (MSG_SAMPLE_META) carrying what it actually applied after clamping -
    // never assume these values were taken verbatim.  // -240..+120 (-24.0 .. +12.0 dB)
    uint32_t start_frame;
    uint32_t end_frame;  // 0 = end of file
    uint32_t loop_start;
    uint32_t loop_end;  // 0 = end_frame

    SampleEditMessage()
        : slot(0),
          loop_enabled(0),
          gain_db_x10(0),
          start_frame(0),
          end_frame(0),
          loop_start(0),
          loop_end(0) {}
    SampleEditMessage(uint8_t slot_,
                      uint8_t loop_enabled_,
                      int16_t gain_db_x10_,
                      uint32_t start_frame_,
                      uint32_t end_frame_,
                      uint32_t loop_start_,
                      uint32_t loop_end_)
        : slot(slot_),
          loop_enabled(loop_enabled_),
          gain_db_x10(gain_db_x10_),
          start_frame(start_frame_),
          end_frame(end_frame_),
          loop_start(loop_start_),
          loop_end(loop_end_) {}
} __attribute__((packed));

// Diagnostics subscription (frontend -> backend).
struct DiagSubscribeMessage {
    uint8_t enable;       // 1 = push while subscribed, 0 = stop
    uint8_t interval_hz;  // pushes per second, clamped 1..10 by the backend
    uint8_t reserved[2];

    DiagSubscribeMessage() : enable(0), interval_hz(2), reserved{0, 0} {}
    DiagSubscribeMessage(uint8_t enable_, uint8_t interval_hz_)
        : enable(enable_), interval_hz(interval_hz_), reserved{0, 0} {}
} __attribute__((packed));

// Diagnostics telemetry (backend -> frontend, unsolicited while subscribed).
//
// Counters are DELTAS over interval_ms and reset on read, matching the
// convention the Daisy's SD PERF / UART PERF telemetry already uses. A
// since-boot total hides a fault that started thirty seconds ago - during the
// August 2026 audition debugging a frozen read count was misread as "idle"
// when in fact every read was failing. Absolute values are used only for
// levels and states, where a delta would be meaningless.
struct DiagPushMessage {
    // --- audio ---
    uint16_t callback_hz_x10;   // 10000 = 1000.0 Hz; separates "engine
                                // stopped" from "ring starved", which is
                                // otherwise undetectable: if the callback
                                // stops, the ring stays full and no
                                // underrun is ever logged
    uint16_t ring_low_water;    // frames, of RB_CAP_FRAMES
    uint16_t underruns;         // episodes this interval
    uint16_t prebuffer_filled;  // frames, of PREBUFFER_FRAMES
    uint16_t engine_cpu_x10;    // average over the interval
    uint16_t engine_cpu_max_x10;
    uint32_t wav_sample_rate;
    uint8_t wav_channels;
    uint8_t wav_bits;
    uint8_t playing;
    uint8_t resampling;  // ratio != 1.0
    uint32_t ring_pushes;
    uint32_t ring_discards;  // skip-without-consume; invisible in every other
                             // figure, and how two playback stalls began

    // --- storage ---
    uint8_t sd_mounted;
    uint8_t sd_speed_index;  // 0..4; the speed names live on the frontend
    uint16_t sd_reads;
    uint32_t sd_bytes;
    uint16_t sd_lat_avg_us;
    uint16_t sd_lat_max_us;  // latency creeping before errors appear is the
                             // marginal-timing tell
    uint16_t sd_errors;
    uint16_t sd_recoveries;
    uint8_t sd_last_fatfs;  // FRESULT
    uint8_t reserved0;
    uint32_t sd_hal_err;  // HAL_SD_GetError(); carried alongside the FRESULT
                          // because FR_DISK_ERR alone says only "the read
                          // failed", where SDMMC_ERROR_DATA_CRC_FAIL with the
                          // card in TRANSFER state says "card healthy,
                          // wiring marginal" - a different action entirely
    uint32_t sample_ram_free;
    uint32_t sample_ram_largest;
    uint16_t sample_failed_allocs;
    uint16_t sample_count;

    // --- link (the backend's own view; the frontend keeps its own) ---
    uint32_t link_total_us;  // time spent in UartLinkProcess this interval
    uint16_t link_max_us;
    uint16_t link_rx_frames;
    uint16_t link_tx_frames;
    uint16_t link_errors;
    uint16_t link_seq_drops;
    uint16_t link_queue_overflows;

    // --- midi / transport ---
    uint16_t midi_notes;
    uint16_t midi_ccs;
    uint16_t midi_clock_ticks;
    uint16_t measured_bpm_x100;
    uint8_t sync_state;  // 0=internal 1=acquiring 2=locked 3=freewheel
    uint8_t transport_playing;
    uint8_t pattern;
    uint8_t step;

    uint32_t interval_ms;  // the window these deltas cover

    // Zeroing default constructor only. The named-argument constructor the
    // other wire structs carry exists to force call sites to be re-checked
    // when a field moves; with ~40 fields filled one at a time by a collector
    // that would be unreadable and would not achieve that. Fields are assigned
    // by name instead, which fails loudly on a rename and ignores reordering.
    DiagPushMessage() { detail::ZeroWire(this, sizeof(*this)); }
} __attribute__((packed));

// Error message (short)
struct ErrorMessage {
    uint16_t code;
    char msg[48];

    ErrorMessage() : code(0) { msg[0] = '\0'; }
    ErrorMessage(uint16_t code_, const char* msg_) : code(code_) {
        detail::CopyWireString(msg, sizeof(msg), msg_);
    }
} __attribute__((packed));

// Acknowledgment message
struct AckMessage {
    uint16_t serial_id;  // Serial ID of the message being acknowledged

    AckMessage() : serial_id(0) {}
    explicit AckMessage(uint16_t serial_id_) : serial_id(serial_id_) {}
} __attribute__((packed));

// Sync message (simple keepalive)
struct SyncMessage {
    uint32_t timestamp_ms;  // Timestamp for sync
    uint8_t reserved[4];    // Reserved for future use

    SyncMessage() : timestamp_ms(0), reserved{0, 0, 0, 0} {}
    explicit SyncMessage(uint32_t timestamp_ms_)
        : timestamp_ms(timestamp_ms_), reserved{0, 0, 0, 0} {}
} __attribute__((packed));

// Heartbeat (bidirectional) - extended with CPU usage
struct HeartbeatMessage {
    uint32_t uptime_ms;
    uint32_t rx_total;
    uint32_t loop_counter;
    uint16_t cpu_avg_percent;  // Average CPU usage as percentage * 10 (e.g., 25.6% = 256)
    uint16_t cpu_min_percent;  // Minimum CPU usage as percentage * 10
    uint16_t cpu_max_percent;  // Maximum CPU usage as percentage * 10

    HeartbeatMessage()
        : uptime_ms(0),
          rx_total(0),
          loop_counter(0),
          cpu_avg_percent(0),
          cpu_min_percent(0),
          cpu_max_percent(0) {}
    HeartbeatMessage(uint32_t uptime_ms_,
                     uint32_t rx_total_,
                     uint32_t loop_counter_,
                     uint16_t cpu_avg_percent_ = 0,
                     uint16_t cpu_min_percent_ = 0,
                     uint16_t cpu_max_percent_ = 0)
        : uptime_ms(uptime_ms_),
          rx_total(rx_total_),
          loop_counter(loop_counter_),
          cpu_avg_percent(cpu_avg_percent_),
          cpu_min_percent(cpu_min_percent_),
          cpu_max_percent(cpu_max_percent_) {}
} __attribute__((packed));

// Index-based file selection messages
struct SamplePlayIndexMessage {
    uint32_t index;  // File index in current directory
    // Silence inserted between loop passes, in milliseconds.
    //
    // A property of THIS audition, not of the sample - which is why it rides
    // on the play request rather than on SampleMetadata. The browser asks for
    // a gap so a short file does not sound like a drone and you can hear
    // where it ends; the editor asks for none, because the whole point there
    // is hearing the seam exactly as it will play.
    uint16_t loop_gap_ms;
    uint8_t reserved[2];

    SamplePlayIndexMessage() : index(0), loop_gap_ms(0), reserved{0, 0} {}
    explicit SamplePlayIndexMessage(uint32_t index_, uint16_t loop_gap_ms_ = 0)
        : index(index_), loop_gap_ms(loop_gap_ms_), reserved{0, 0} {}
} __attribute__((packed));

struct SampleGetPathMessage {
    uint32_t index;  // File index in current directory

    SampleGetPathMessage() : index(0) {}
    explicit SampleGetPathMessage(uint32_t index_) : index(index_) {}
} __attribute__((packed));

struct SamplePathResponseMessage {
    uint32_t index;  // File index that was requested
    char path[200];  // Full file path (null-terminated)

    SamplePathResponseMessage() : index(0) { path[0] = '\0'; }
    SamplePathResponseMessage(uint32_t index_, const char* path_) : index(index_) {
        detail::CopyWireString(path, sizeof(path), path_);
    }
} __attribute__((packed));

// Sample memory status entry (Daisy -> ESP32)
struct SampleMemEntryMessage {
    uint16_t sample_id;        // Logical sample identifier
    uint32_t allocated_bytes;  // Bytes reserved in sample RAM
    uint32_t loaded_bytes;     // Bytes written so far
    uint8_t cls;               // Allocation class (0-5 small, 0xFF large)
    uint16_t page;             // Page index (class-relative or extent start)
    uint16_t slot;             // Slot index (small) or page_count (large)
    uint16_t sample_rate;      // Hz
    uint8_t channels;          // 1 or 2
    uint8_t bit_depth;         // e.g., 16

    SampleMemEntryMessage()
        : sample_id(0),
          allocated_bytes(0),
          loaded_bytes(0),
          cls(0),
          page(0),
          slot(0),
          sample_rate(0),
          channels(0),
          bit_depth(0) {}
    SampleMemEntryMessage(uint16_t sample_id_,
                          uint32_t allocated_bytes_,
                          uint32_t loaded_bytes_,
                          uint8_t cls_,
                          uint16_t page_,
                          uint16_t slot_,
                          uint16_t sample_rate_,
                          uint8_t channels_,
                          uint8_t bit_depth_)
        : sample_id(sample_id_),
          allocated_bytes(allocated_bytes_),
          loaded_bytes(loaded_bytes_),
          cls(cls_),
          page(page_),
          slot(slot_),
          sample_rate(sample_rate_),
          channels(channels_),
          bit_depth(bit_depth_) {}
} __attribute__((packed));

// Sample memory status payload (Daisy -> ESP32)
struct SampleMemStatusMessage {
    uint8_t category;      // STATUS_CATEGORY_SAMPLE_MEM
    uint8_t sample_count;  // Number of valid entries
    uint8_t reserved[2];
    uint32_t small_total_bytes;
    uint32_t small_free_bytes;
    uint32_t large_total_bytes;
    uint32_t large_free_bytes;
    uint32_t largest_free_bytes;
    uint32_t in_use_bytes;
    uint32_t failed_allocs;
    SampleMemEntryMessage entries[WAVEX_SAMPLE_STATUS_MAX_ENTRIES];

    // Default-constructs with sample_count=0; fill `entries[0..sample_count)`
    // and call SetSampleCount() before sending.
    SampleMemStatusMessage()
        : category(STATUS_CATEGORY_SAMPLE_MEM),
          sample_count(0),
          reserved{0, 0},
          small_total_bytes(0),
          small_free_bytes(0),
          large_total_bytes(0),
          large_free_bytes(0),
          largest_free_bytes(0),
          in_use_bytes(0),
          failed_allocs(0) {}
    SampleMemStatusMessage(uint32_t small_total_bytes_,
                           uint32_t small_free_bytes_,
                           uint32_t large_total_bytes_,
                           uint32_t large_free_bytes_,
                           uint32_t largest_free_bytes_,
                           uint32_t in_use_bytes_,
                           uint32_t failed_allocs_)
        : category(STATUS_CATEGORY_SAMPLE_MEM),
          sample_count(0),
          reserved{0, 0},
          small_total_bytes(small_total_bytes_),
          small_free_bytes(small_free_bytes_),
          large_total_bytes(large_total_bytes_),
          large_free_bytes(large_free_bytes_),
          largest_free_bytes(largest_free_bytes_),
          in_use_bytes(in_use_bytes_),
          failed_allocs(failed_allocs_) {}

    // Bounds-checked append; returns false (no-op) if entries[] is already full.
    bool AddEntry(const SampleMemEntryMessage& entry) {
        if (sample_count >= WAVEX_SAMPLE_STATUS_MAX_ENTRIES)
            return false;
        entries[sample_count++] = entry;
        return true;
    }
} __attribute__((packed));

// CV calibration for one analog group (Stage A/B - mirrors CvCal in
// firmware/daisy/src/cv/cv_cal.hpp; see analog-voice-board.md §3).
// Used by MSG_CV_CAL_SET (E->D) and MSG_CV_CAL_RESP (D->E).
struct CvCalMessage {
    uint8_t group;        // 0..WAVEX_ANALOG_CV_GROUPS_MAX-1
    uint8_t persist;      // SET only: 1 = also write the table to SD
    uint8_t reserved[2];  // alignment/future
    float vcf_cut_gain;
    float vcf_cut_off;
    float vcf_q_gain;
    float vcf_q_off;
    float vca_gain;
    float vca_off;
    float cutoff_k;  // exponential cutoff-shaping curvature

    CvCalMessage()
        : group(0),
          persist(0),
          reserved{0, 0},
          vcf_cut_gain(1.0f),
          vcf_cut_off(0.0f),
          vcf_q_gain(1.0f),
          vcf_q_off(0.0f),
          vca_gain(1.0f),
          vca_off(0.0f),
          cutoff_k(3.0f) {}
    CvCalMessage(uint8_t group_,
                 uint8_t persist_,
                 float vcf_cut_gain_,
                 float vcf_cut_off_,
                 float vcf_q_gain_,
                 float vcf_q_off_,
                 float vca_gain_,
                 float vca_off_,
                 float cutoff_k_)
        : group(group_),
          persist(persist_),
          reserved{0, 0},
          vcf_cut_gain(vcf_cut_gain_),
          vcf_cut_off(vcf_cut_off_),
          vcf_q_gain(vcf_q_gain_),
          vcf_q_off(vcf_q_off_),
          vca_gain(vca_gain_),
          vca_off(vca_off_),
          cutoff_k(cutoff_k_) {}
} __attribute__((packed));

// Request one group's calibration (MSG_CV_CAL_GET, E->D).
struct CvCalGetMessage {
    uint8_t group;
    uint8_t reserved[3];

    CvCalGetMessage() : group(0), reserved{0, 0, 0} {}
    explicit CvCalGetMessage(uint8_t group_) : group(group_), reserved{0, 0, 0} {}
} __attribute__((packed));

// Calibration-procedure CV override (MSG_CV_TEST, E->D): while enabled the
// control tick stages these fixed values instead of the paraphonic law, so
// the user can measure corner frequencies / verify VCA silence with a
// steady CV. Disable returns control to the envelope.
struct CvTestMessage {
    uint8_t group;
    uint8_t enable;  // 1 = override active, 0 = back to the paraphonic law
    uint8_t reserved[2];
    float cutoff;     // 0..1 pre-calibration control values
    float resonance;  // 0..1
    float vca;        // 0..1

    CvTestMessage()
        : group(0), enable(0), reserved{0, 0}, cutoff(0.0f), resonance(0.0f), vca(0.0f) {}
    CvTestMessage(uint8_t group_, uint8_t enable_, float cutoff_, float resonance_, float vca_)
        : group(group_),
          enable(enable_),
          reserved{0, 0},
          cutoff(cutoff_),
          resonance(resonance_),
          vca(vca_) {}
} __attribute__((packed));

// ---- Sequencer / transport / MIDI clock (Phase 2) ----
// See docs/features/sequencer.md §4, midi-sync-tempo-follower.md §3,
// melodic-sequencing.md §4. The engine-internal Pattern model lives on the
// Daisy (firmware/daisy/src/sequencer/pattern.hpp) and is NOT sent verbatim;
// these are the small idempotent edit/feedback ops that drive it.

enum SeqTransportCmd : uint8_t {
    SEQ_TRANSPORT_STOP = 0,
    SEQ_TRANSPORT_PLAY = 1,      // start from the top (resets playhead)
    SEQ_TRANSPORT_CONTINUE = 2,  // resume from song_position (SPP-style)
};
enum SeqClockSource : uint8_t {
    SEQ_CLOCK_INTERNAL = 0,
    SEQ_CLOCK_MIDI = 1,
};
enum SeqInputMode : uint8_t {
    SEQ_INPUT_PLAY = 0,
    SEQ_INPUT_STEP_RECORD = 1,
    SEQ_INPUT_LIVE_RECORD = 2,
    SEQ_INPUT_LIVE_ERASE = 3,
};

// MSG_SEQ_TRANSPORT (E->D): transport + global sequencer mode. tempo_bpm_x100
// is BPM*100 (12000 = 120.00 BPM); song_position is in MIDI beats (16th notes)
// and is only consulted for SEQ_TRANSPORT_CONTINUE.
struct SeqTransportMessage {
    uint8_t command;       // SeqTransportCmd
    uint8_t clock_source;  // SeqClockSource
    uint8_t input_mode;    // SeqInputMode
    uint8_t quantize;      // 0=off, 1=step, 2=half-step (live-record capture)
    uint16_t tempo_bpm_x100;
    uint16_t song_position;

    SeqTransportMessage()
        : command(0),
          clock_source(0),
          input_mode(0),
          quantize(0),
          tempo_bpm_x100(12000),
          song_position(0) {}
    SeqTransportMessage(uint8_t command_,
                        uint8_t clock_source_,
                        uint8_t input_mode_,
                        uint8_t quantize_,
                        uint16_t tempo_bpm_x100_,
                        uint16_t song_position_)
        : command(command_),
          clock_source(clock_source_),
          input_mode(input_mode_),
          quantize(quantize_),
          tempo_bpm_x100(tempo_bpm_x100_),
          song_position(song_position_) {}
} __attribute__((packed));

// MSG_SEQ_PATTERN_OP (E->D): one small idempotent pattern edit. The op field
// selects which of track/step/arg_* are meaningful:
//   op                     | track | step | arg_u8            | arg_u16          | arg_s16
//   SEQ_OP_SET_STEP        |  yes  | yes  | on(0/1)           | velocity(0..127) | -
//   SEQ_OP_TOGGLE_STEP     |  yes  | yes  | -                 | -                | -
//   SEQ_OP_SET_STEP_PROB   |  yes  | yes  | probability(0..100)| -               | -
//   SEQ_OP_SET_STEP_MICRO  |  yes  | yes  | retrig_count      | retrig_rate_ticks| micro_offset
//   SEQ_OP_TRACK_MUTE      |  yes  |  -   | enabled(0/1)      | -                | -
//   SEQ_OP_PATTERN_LENGTH  |   -   |  -   | -                 | length(1..64)    | -
//   SEQ_OP_PATTERN_SCALE   |   -   |  -   | scale(StepScale)  | -                | -
//   SEQ_OP_PATTERN_SWING   |   -   |  -   | swing(50..75)     | -                | -
//   SEQ_OP_SET_PARAM_LOCK  |  yes  | yes  | param_id(slot key)| value            | -
//   SEQ_OP_CLEAR_PARAM_LOCKS| yes  | yes  | -                 | -                | -
enum SeqPatternOpCode : uint8_t {
    SEQ_OP_SET_STEP = 0,
    SEQ_OP_TOGGLE_STEP = 1,
    SEQ_OP_SET_STEP_PROB = 2,
    SEQ_OP_SET_STEP_MICRO = 3,
    SEQ_OP_TRACK_MUTE = 4,
    SEQ_OP_PATTERN_LENGTH = 5,
    SEQ_OP_PATTERN_SCALE = 6,
    SEQ_OP_PATTERN_SWING = 7,
    SEQ_OP_SET_PARAM_LOCK = 8,
    SEQ_OP_CLEAR_PARAM_LOCKS = 9,
};
struct SeqPatternOpMessage {
    uint8_t op;      // SeqPatternOpCode
    uint8_t track;   // 0..15 (ignored for pattern-scoped ops)
    uint8_t step;    // 0..63 (ignored for track/pattern-scoped ops)
    uint8_t arg_u8;  // op-dependent (see table above)
    uint16_t arg_u16;
    int16_t arg_s16;

    SeqPatternOpMessage() : op(0), track(0), step(0), arg_u8(0), arg_u16(0), arg_s16(0) {}
    SeqPatternOpMessage(uint8_t op_,
                        uint8_t track_,
                        uint8_t step_,
                        uint8_t arg_u8_,
                        uint16_t arg_u16_,
                        int16_t arg_s16_)
        : op(op_),
          track(track_),
          step(step_),
          arg_u8(arg_u8_),
          arg_u16(arg_u16_),
          arg_s16(arg_s16_) {}
} __attribute__((packed));

// MSG_SEQ_PLAYHEAD (D->E): coalesced playhead + sync feedback for the UI.
// measured_bpm_x100 mirrors SeqTransportMessage's tempo encoding.
struct SeqPlayheadMessage {
    uint8_t pattern;     // active pattern index
    uint8_t step;        // current step 0..63
    uint8_t playing;     // 0=stopped, 1=playing
    uint8_t sync_state;  // 0=internal,1=acquiring,2=locked,3=freewheel
    uint16_t measured_bpm_x100;
    uint32_t loop_count;  // patterns elapsed since transport start

    SeqPlayheadMessage()
        : pattern(0), step(0), playing(0), sync_state(0), measured_bpm_x100(12000), loop_count(0) {}
    SeqPlayheadMessage(uint8_t pattern_,
                       uint8_t step_,
                       uint8_t playing_,
                       uint8_t sync_state_,
                       uint16_t measured_bpm_x100_,
                       uint32_t loop_count_)
        : pattern(pattern_),
          step(step_),
          playing(playing_),
          sync_state(sync_state_),
          measured_bpm_x100(measured_bpm_x100_),
          loop_count(loop_count_) {}
} __attribute__((packed));

enum MidiClockEventType : uint8_t {
    MIDI_CLK_TICK = 0,      // 0xF8
    MIDI_CLK_START = 1,     // 0xFA
    MIDI_CLK_CONTINUE = 2,  // 0xFB
    MIDI_CLK_STOP = 3,      // 0xFC
    MIDI_CLK_SPP = 4,       // 0xF2 (song position pointer)
};
// MSG_MIDI_CLOCK_EVENT (E->D): a forwarded MIDI real-time/transport byte. Per
// midi-sync-tempo-follower.md §2/§3 the ESP32 sends the DELTA since the
// previous event from this source (never an absolute foreign timestamp), so
// the Daisy's tempo follower cannot mix clock domains.
struct MidiClockEventMessage {
    uint8_t event;          // MidiClockEventType
    uint8_t source;         // 0=DIN, 1=USB
    uint16_t tick_seq;      // wraps; gap detection for dropped ticks
    uint32_t esp_delta_us;  // us since the previous event from this source (0 on first/START)
    uint16_t spp_beats16;   // SPP payload in MIDI beats (16th notes); event==MIDI_CLK_SPP only
    uint16_t reserved;

    MidiClockEventMessage()
        : event(0), source(0), tick_seq(0), esp_delta_us(0), spp_beats16(0), reserved(0) {}
    MidiClockEventMessage(uint8_t event_,
                          uint8_t source_,
                          uint16_t tick_seq_,
                          uint32_t esp_delta_us_,
                          uint16_t spp_beats16_)
        : event(event_),
          source(source_),
          tick_seq(tick_seq_),
          esp_delta_us(esp_delta_us_),
          spp_beats16(spp_beats16_),
          reserved(0) {}
} __attribute__((packed));

// MSG_MIDI_CC (E->D): a forwarded MIDI control change. The Daisy owns the
// CC->modulation-source mapping (param-locks-and-modulation.md §6).
struct MidiCcMessage {
    uint8_t cc;
    uint8_t value;
    uint8_t channel;
    uint8_t reserved;

    MidiCcMessage() : cc(0), value(0), channel(0), reserved(0) {}
    MidiCcMessage(uint8_t cc_, uint8_t value_, uint8_t channel_)
        : cc(cc_), value(value_), channel(channel_), reserved(0) {}
} __attribute__((packed));

// MSG_SEQ_CLOCK_OUT (D->E): the Daisy is the timing master; the ESP32 turns
// these into 0xF8/0xFA/... bytes on DIN + USB immediately (no TX coalescing).
struct SeqClockOutMessage {
    uint8_t event;  // MidiClockEventType
    uint8_t reserved;
    uint16_t tick_seq;
    uint16_t spp_beats16;  // event==MIDI_CLK_SPP only
    uint16_t reserved2;

    SeqClockOutMessage() : event(0), reserved(0), tick_seq(0), spp_beats16(0), reserved2(0) {}
    SeqClockOutMessage(uint8_t event_, uint16_t tick_seq_, uint16_t spp_beats16_)
        : event(event_),
          reserved(0),
          tick_seq(tick_seq_),
          spp_beats16(spp_beats16_),
          reserved2(0) {}
} __attribute__((packed));

// Legacy packet structures completely removed - using new simplified format only

// Maximum packet size support
static const size_t MAX_PKT_SIZE = 2048;

// Protocol functions
class ProtocolHandler {
   public:
    // Packet creation
    static size_t CreateControlChangePacket(
        uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value);

    static size_t CreateNoteOnPacket(
        uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel);

    static size_t CreateNoteOffPacket(uint8_t* buffer,
                                      size_t buffer_size,
                                      uint8_t note,
                                      uint8_t channel);

    // Phase I: additional packet creators
    static size_t CreateSampleCtrlPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         const SampleCtrlMessage& msg);

    static size_t CreatePreviewReqPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         const PreviewReqMessage& msg);

    static size_t CreateDataRequestPacket(uint8_t* buffer,
                                          size_t buffer_size,
                                          const DataRequestMessage& msg);

    static size_t CreateMeterPushPacket(uint8_t* buffer,
                                        size_t buffer_size,
                                        const MeterPushMessage& msg);

    static size_t CreateWaveChunkPacket(uint8_t* buffer,
                                        size_t buffer_size,
                                        const WaveChunkMessage& msg,
                                        const void* sample_data,
                                        size_t sample_data_size);

    static size_t CreateSamplePathResponsePacket(uint8_t* buffer,
                                                 size_t buffer_size,
                                                 const SamplePathResponseMessage& msg);

    // Additional flexible packet system functions
    static size_t CreateErrorPacket(uint8_t* buffer, size_t buffer_size, const ErrorMessage& err);
    static size_t CreateSampleStatusPacket(uint8_t* buffer,
                                           size_t buffer_size,
                                           const SampleStatusMessage& msg);
    /** Storage availability notification (backend -> frontend, unsolicited). */
    static size_t CreateStorageStatusPacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            const StorageStatusMessage& status);

    static size_t CreateSampleStopRespPacket(uint8_t* buffer,
                                             size_t buffer_size,
                                             const SampleStopRespMessage& msg);
    static size_t CreateSampleStopReqPacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            const SampleStopReqMessage& msg);
    static size_t CreateBrowseRespPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         uint32_t total_count,
                                         const FileEntryWire* entries,
                                         uint8_t n);
    static size_t CreateSyncPacket(uint8_t* buffer, size_t buffer_size, const SyncMessage& msg);
    static size_t CreateHeartbeatPacket(uint8_t* buffer,
                                        size_t buffer_size,
                                        const HeartbeatMessage& msg);
    static size_t CreateAckPacket(uint8_t* buffer, size_t buffer_size, const AckMessage& ack);
    static size_t CreateSamplePlayIndexPacket(uint8_t* buffer,
                                              size_t buffer_size,
                                              const SamplePlayIndexMessage& msg);
    static size_t CreateSampleGetPathPacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            const SampleGetPathMessage& msg);
    /** Authoritative per-sample record (backend -> frontend). */
    static size_t CreateSampleMetaPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         const SampleMetadata& msg);
    /** Metadata resend request (frontend -> backend). */
    static size_t CreateSampleMetaReqPacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            const SampleMetaReqMessage& msg);

    /** Non-destructive playback edit (frontend -> backend). */
    static size_t CreateSampleEditPacket(uint8_t* buffer,
                                         size_t buffer_size,
                                         const SampleEditMessage& msg);

    /** Diagnostics subscription (frontend -> backend). */
    static size_t CreateDiagSubscribePacket(uint8_t* buffer,
                                            size_t buffer_size,
                                            const DiagSubscribeMessage& msg);
    /** Diagnostics telemetry (backend -> frontend, unsolicited). */
    static size_t CreateDiagPushPacket(uint8_t* buffer,
                                       size_t buffer_size,
                                       const DiagPushMessage& msg);

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
    static bool ParseMessage(const uint8_t* buffer,
                             MessageType expected_type,
                             void* out_payload,
                             size_t out_payload_size);

    // New simplified packet system functions
    static size_t GetPacketSizeFromCode(uint8_t size_code);
    static uint8_t GetOptimalSizeCode(size_t payload_size);
    // Core packet creation with automatic sequence number management
    static size_t CreatePacket(uint8_t* buffer,
                               size_t buffer_size,
                               uint8_t msg_type,
                               const void* payload,
                               size_t payload_size,
                               uint8_t flags = 0);

    static size_t CreateWaveXPacket(uint8_t* buffer,
                                    size_t buffer_size,
                                    uint8_t msg_type,
                                    const void* payload,
                                    size_t payload_size,
                                    uint16_t sequence_number = 0,
                                    uint8_t flags = 0);
    static bool ParseWaveXPacket(const uint8_t* buffer,
                                 size_t buffer_size,
                                 uint8_t& msg_type,
                                 void* payload,
                                 size_t& payload_size,
                                 uint16_t& sequence_number,
                                 uint8_t& flags);
    static uint16_t CalculateWaveXCrc(const uint8_t* data, size_t length);
    static uint16_t CalculateSpiCrc(const uint8_t* data, size_t length);  // Legacy compatibility
    static uint16_t CalculatePacketCrc(const uint8_t* packet_data,
                                       size_t packet_size);  // Legacy compatibility
    static bool ValidateWaveXPacket(const uint8_t* buffer, size_t buffer_size);
    static bool ValidatePacketCrc(const uint8_t* packet_data,
                                  size_t packet_size);  // Legacy compatibility
    static void ZeroPadPacket(uint8_t* packet_data,
                              size_t packet_size,
                              size_t used_size);  // Legacy compatibility

    // Utility functions
    static uint8_t CalculateChecksum(const uint8_t* data, size_t length);
    static size_t GetPacketSize(const uint8_t* buffer);
};

}  // namespace Protocol
}  // namespace WaveX

#endif  // WAVEX_PROTOCOL_H
