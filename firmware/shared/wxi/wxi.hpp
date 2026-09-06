#pragma once

// .wxi Instrument file codec over the WXCF container (design:
// docs/features/track-and-patch-model.md §3.3; container:
// docs/features/instrument-model.md §5, firmware/shared/wxcf/wxcf.hpp).
//
// This header owns the *wire* form of an Instrument, not the engine's.
// `audio/instrument.hpp`'s structs are deliberately runtime-shaped - a Zone
// there names its Sample by Pool id, which is meaningless across a power
// cycle - so the file model below names Samples by card path and carries the
// fixed-width fields §5 asks for. It lives in firmware/shared/ rather than
// beside the engine so it round-trips on host without SDRAM or the audio HAL,
// and so the ESP32 side can read the same files when the Instrument Browser
// needs to show what is in one without loading it.
//
// The file model is also *ahead* of the engine model, on purpose: it holds
// the two typed oscillators, the instrument-level filter/amp/envelopes and
// the per-voice LFOs that track-and-patch-model.md §3.1 specifies but Phase
// 2.5 stage 5 has not built yet. Files written today therefore already have a
// place for those values, which is the whole point of settling the chunk
// layout before the fields exist.
//
// Forward compatibility, three layers deep:
//   - Unknown chunk_ids are skipped (WXCF's own rule).
//   - A known chunk whose payload is LONGER than this version understands has
//     its trailing bytes skipped, so a later writer's extra fields do not
//     break this reader.
//   - Repeated records (Zones, mod rows) carry an explicit stride, so a
//     future wider Zone is walked correctly rather than misaligned.
// A known chunk whose payload is SHORTER than this version's fixed part is
// treated as corruption, not as a defaulted field: chunk ids and payload
// widths only ever grow, so a short one cannot be a legitimate older file -
// it can only be a bad read, and a bad read must not load as a silent
// half-Instrument.
//
// Atomicity ("write <name>.tmp, f_close, f_rename") belongs to the FatFs
// wrapper, exactly as it does for wxcf.hpp - nothing here knows about files.

#include "wxcf/wxcf.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace Wxi {

// WXCF header's file_type for an Instrument (instrument-model.md §5).
static constexpr uint16_t kFileType = 1;
// Major 1: a reader rejects a major it does not know. Minor bumps are
// additive field/chunk growth, which the skip rules above already absorb.
static constexpr uint16_t kFileVersion = 0x0100;  // 1.0

// Matches AudioEngine::kInstrumentNameBytes and TrackBindingMessage::name - a
// name that survives a save must survive the trip to the frontend too.
static constexpr size_t kNameBytes = 24;
// Matches Protocol::BROWSE_PATH_MAX: a Zone's Sample path is exactly what the
// browser hands the loader, so the two cannot disagree about how long a path
// may be.
static constexpr size_t kPathBytes = 96;

static constexpr uint8_t kMaxZonesPerOsc = 32;  // == AudioEngine::kMaxZones
static constexpr uint8_t kNumOscillators = 2;
static constexpr uint8_t kNumEnvelopes = 3;
static constexpr uint8_t kNumVoiceLfos = 2;
static constexpr uint8_t kMaxModSlots = 8;  // == AudioEngine::kMaxModSlots

// Chunk ids. Wire-stable: never renumber, never reuse a retired id. Grouped
// by decade so a new member of a group lands next to its siblings.
enum ChunkId : uint16_t {
    kChunkHead = 0x0001,
    kChunkOsc1 = 0x0010,
    kChunkOsc2 = 0x0011,
    kChunkFilt = 0x0020,
    kChunkAmp = 0x0021,
    kChunkEnv1 = 0x0030,
    kChunkEnv2 = 0x0031,
    kChunkEnv3 = 0x0032,
    kChunkLfo1 = 0x0040,
    kChunkLfo2 = 0x0041,
    kChunkModm = 0x0050,
    // Reserved for the FX chain (§3.1, "Reserved, not fields yet"). Never
    // written today; a reader that meets one skips it. Reserving the id now
    // is what lets a file with FX load on firmware that has no FX.
    kChunkFxch = 0x0060,
};

static constexpr uint16_t kChunkVersion = 0x0100;  // every chunk is 1.0 today

enum class Result : uint8_t {
    Ok,
    IoError,       // a WXCF callback failed (includes EOF mid-structure)
    BadMagic,      // not a WXCF file at all
    BadFileType,   // a WXCF file, but not an Instrument
    BadVersion,    // an Instrument file from a future major
    BadChunk,      // a chunk we understand is truncated or self-inconsistent
    TooManyZones,  // more zones in the file than this build can hold
};

// Mirrors AudioEngine::InstrumentMode.
enum class Mode : uint8_t { Keyboard = 0, Drum = 1 };

// track-and-patch-model.md §3.1. `Wavetable` is reserved (oscillator-sources.md
// defines its body); an oscillator whose type this build cannot render loads
// as Off rather than failing, so one unknown oscillator never costs the user
// the rest of the Instrument.
enum class OscType : uint8_t { Off = 0, Sample = 1, Wavetable = 2 };

// track-and-patch-model.md §3.1: a 3-bit field, 8 values reserved, 4 defined.
enum class FilterType : uint8_t { SvfLp = 0, SvfHp = 1, SvfBp = 2, SvfNotch = 3 };

// ---------------------------------------------------------------------------
// File model
// ---------------------------------------------------------------------------

// One Zone as stored. Field-for-field the engine's Zone (instrument.hpp)
// except that `sample_id` becomes `path` and `in_use` becomes membership in
// the written array. `index` preserves the zone's slot in the engine's sparse
// array, so "zone 3" stays zone 3 across a save/load even when zones 0-2 are
// empty - an editor that addresses zones by index would otherwise renumber
// the user's work on every save.
struct Zone {
    uint8_t index = 0;
    char path[kPathBytes] = {};
    uint8_t key_lo = 0, key_hi = 127;
    uint8_t vel_lo = 1, vel_hi = 127;
    uint8_t root_note = 60;
    int8_t coarse_tune = 0;  // semitones
    int8_t fine_tune = 0;    // cents
    float gain = 1.0f;       // linear, pre-velocity
    float pan = 0.5f;
    uint32_t start_frame = 0, end_frame = 0;
    uint32_t loop_start = 0, loop_end = 0;
    uint8_t loop_mode = 0;    // AudioEngine::ZoneLoopMode
    uint8_t choke_group = 0;  // 0 = none
    uint8_t output_bus = 0;
    uint8_t flags = 0;  // AudioEngine::ZoneFlags
    float cutoff_hz = 20000.0f;
    float attack_s = 0.001f, decay_s = 0.05f, sustain = 0.8f, release_s = 0.1f;
};

struct Oscillator {
    OscType type = OscType::Off;
    float level = 1.0f;
    float pan = 0.5f;
    int8_t coarse_tune = 0;
    int8_t fine_tune = 0;
    uint8_t keytrack = 1;  // 1 = pitch follows the note; 0 = fixed (pads)
    uint8_t zone_count = 0;
    Zone zones[kMaxZonesPerOsc];
};

struct Filter {
    FilterType type = FilterType::SvfLp;
    float cutoff_hz = 20000.0f;
    float resonance = 0.0f;
    float keytrack = 0.0f;
    float env2_amount = 0.0f;
};

struct Amp {
    uint8_t velocity_curve = 0;
};

struct Adsr {
    float attack_s = 0.001f, decay_s = 0.05f, sustain = 0.8f, release_s = 0.1f;
};

struct LfoParams {
    uint8_t wave = 0;      // AudioEngine::LfoWave
    float rate_hz = 1.0f;  // used when sync_div == 0
    uint8_t sync_div = 0;  // 0 = free-running at rate_hz; else a tempo division
    float delay_s = 0.0f;  // the E-mu delayed-vibrato shape
    float fade_s = 0.0f;
    uint8_t retrigger = 1;  // 1 = restart phase per voice
};

// Mirrors AudioEngine::ModSlot.
struct ModSlot {
    uint8_t source = 0;  // SRC_NONE
    uint8_t dest = 0;    // DEST_NONE
    int16_t depth = 0;   // +-32767 maps to +-100%
    uint8_t curve = 0;   // CURVE_LINEAR
    uint8_t flags = 0;
};

// A whole Instrument as it exists on the card. ~10 KB, dominated by the two
// oscillators' 32 zone paths each, so it is a resident buffer on the Daisy -
// the InstrumentLoader's working document - never a stack local.
struct InstrumentFile {
    char name[kNameBytes] = {};
    uint8_t tags = 0;  // bitmask, §3.4
    Mode mode = Mode::Keyboard;
    int8_t transpose = 0;
    int8_t fine_tune = 0;
    float trim_gain = 1.0f;
    float trim_pan = 0.5f;
    uint8_t output = 0;     // 0 = the Track's stereo bus
    uint8_t poly_mode = 0;  // 0 = poly, 1 = mono, 2 = legato
    float osc_mix = 0.0f;   // 0 = osc 1 only ... 1 = osc 2 only

    Oscillator osc[kNumOscillators];
    Filter filter;
    Amp amp;
    Adsr env[kNumEnvelopes];  // 0 = amp (hard-wired), 1 = filter, 2 = pitch
    LfoParams lfo[kNumVoiceLfos];
    ModSlot mod_slots[kMaxModSlots];
};

// ---------------------------------------------------------------------------
// Wire sizes
// ---------------------------------------------------------------------------

static constexpr uint32_t kHeadWireSize = 42;
static constexpr uint32_t kOscHeaderWireSize = 17;
static constexpr uint32_t kZoneWireSize = 152;
static constexpr uint32_t kFiltWireSize = 17;
static constexpr uint32_t kAmpWireSize = 1;
static constexpr uint32_t kEnvWireSize = 16;
static constexpr uint32_t kLfoWireSize = 15;
static constexpr uint32_t kModmHeaderWireSize = 2;
static constexpr uint32_t kModSlotWireSize = 6;

// The largest single read or write the codec performs, so its scratch buffer
// is one fixed, stack-safe size rather than a whole chunk (an oscillator
// chunk is ~5 KB and must not land on the Daisy's stack).
static constexpr size_t kScratchBytes = 192;

// ---------------------------------------------------------------------------
// Scalar codecs
// ---------------------------------------------------------------------------

namespace detail {

using Wxcf::detail::ReadU16LE;
using Wxcf::detail::ReadU32LE;
using Wxcf::detail::WriteU16LE;
using Wxcf::detail::WriteU32LE;

inline void WriteF32LE(uint8_t* dest, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteU32LE(dest, bits);
}

// A float read from a card is untrusted input, and NaN or infinity both
// propagate silently through the audio path into a stuck or silent voice with
// no error anywhere. instrument-model.md §5 asks for exactly this check ("no
// floats-with-NaN risk (validate on read)"). A non-finite or out-of-range
// value becomes the field's own default rather than failing the load, because
// one bad gain should not cost the user the whole Instrument.
inline float ReadF32LE(const uint8_t* src, float lo, float hi, float fallback) {
    const uint32_t bits = ReadU32LE(src);
    if ((bits & 0x7F800000u) == 0x7F800000u)  // exponent all-ones: NaN or Inf
        return fallback;
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    if (v < lo || v > hi)
        return fallback;
    return v;
}

inline void WriteI8(uint8_t* dest, int8_t v) {
    *dest = static_cast<uint8_t>(v);
}
inline int8_t ReadI8(const uint8_t* src) {
    return static_cast<int8_t>(*src);
}
inline void WriteI16LE(uint8_t* dest, int16_t v) {
    WriteU16LE(dest, static_cast<uint16_t>(v));
}
inline int16_t ReadI16LE(const uint8_t* src) {
    return static_cast<int16_t>(ReadU16LE(src));
}

// NUL-pads to the full field width: a fixed-width field left with trailing
// garbage would leak whatever was in the writer's buffer into the file.
inline void WriteFixedString(uint8_t* dest, size_t field_bytes, const char* src) {
    size_t i = 0;
    if (src) {
        for (; i + 1 < field_bytes && src[i] != '\0'; ++i)
            dest[i] = static_cast<uint8_t>(src[i]);
    }
    for (; i < field_bytes; ++i)
        dest[i] = 0;
}

// Always terminates, whatever the file held - an unterminated fixed field is
// the classic way a corrupt card turns into an over-read.
inline void ReadFixedString(char* dest, size_t field_bytes, const uint8_t* src) {
    size_t i = 0;
    for (; i + 1 < field_bytes; ++i)
        dest[i] = static_cast<char>(src[i]);
    dest[i] = '\0';
}

// Accepted ranges for the validator above. Deliberately generous - these
// reject nonsense (a negative gain, a 1e30 cutoff), not unusual-but-intended
// settings, which is why e.g. release runs to ten minutes.
static constexpr float kGainMax = 64.0f;       // linear, ~+36 dB
static constexpr float kCutoffMax = 96000.0f;  // above any supported rate's Nyquist
static constexpr float kTimeMax = 600.0f;      // seconds, for envelope stages
static constexpr float kRateMax = 1000.0f;     // Hz, for an LFO
static constexpr float kAmountMax = 16.0f;     // bipolar modulation depths

}  // namespace detail

// ---------------------------------------------------------------------------
// Chunk encode / decode
// ---------------------------------------------------------------------------

namespace detail {

inline void EncodeHead(const InstrumentFile& doc, uint8_t* b) {
    WriteFixedString(b + 0, kNameBytes, doc.name);
    b[24] = doc.tags;
    b[25] = static_cast<uint8_t>(doc.mode);
    WriteI8(b + 26, doc.transpose);
    WriteI8(b + 27, doc.fine_tune);
    WriteF32LE(b + 28, doc.trim_gain);
    WriteF32LE(b + 32, doc.trim_pan);
    b[36] = doc.output;
    b[37] = doc.poly_mode;
    WriteF32LE(b + 38, doc.osc_mix);
}

inline void DecodeHead(const uint8_t* b, InstrumentFile& doc) {
    ReadFixedString(doc.name, kNameBytes, b + 0);
    doc.tags = b[24];
    doc.mode = (b[25] == 1) ? Mode::Drum : Mode::Keyboard;
    doc.transpose = ReadI8(b + 26);
    doc.fine_tune = ReadI8(b + 27);
    doc.trim_gain = ReadF32LE(b + 28, 0.0f, kGainMax, 1.0f);
    doc.trim_pan = ReadF32LE(b + 32, 0.0f, 1.0f, 0.5f);
    doc.output = b[36];
    doc.poly_mode = b[37];
    doc.osc_mix = ReadF32LE(b + 38, 0.0f, 1.0f, 0.0f);
}

inline void EncodeZone(const Zone& z, uint8_t* b) {
    b[0] = z.index;
    WriteFixedString(b + 1, kPathBytes, z.path);
    b[97] = z.key_lo;
    b[98] = z.key_hi;
    b[99] = z.vel_lo;
    b[100] = z.vel_hi;
    b[101] = z.root_note;
    WriteI8(b + 102, z.coarse_tune);
    WriteI8(b + 103, z.fine_tune);
    WriteF32LE(b + 104, z.gain);
    WriteF32LE(b + 108, z.pan);
    WriteU32LE(b + 112, z.start_frame);
    WriteU32LE(b + 116, z.end_frame);
    WriteU32LE(b + 120, z.loop_start);
    WriteU32LE(b + 124, z.loop_end);
    b[128] = z.loop_mode;
    b[129] = z.choke_group;
    b[130] = z.output_bus;
    b[131] = z.flags;
    WriteF32LE(b + 132, z.cutoff_hz);
    WriteF32LE(b + 136, z.attack_s);
    WriteF32LE(b + 140, z.decay_s);
    WriteF32LE(b + 144, z.sustain);
    WriteF32LE(b + 148, z.release_s);
}

inline void DecodeZone(const uint8_t* b, Zone& z) {
    z.index = b[0];
    ReadFixedString(z.path, kPathBytes, b + 1);
    z.key_lo = b[97];
    z.key_hi = b[98];
    z.vel_lo = b[99];
    z.vel_hi = b[100];
    z.root_note = b[101];
    z.coarse_tune = ReadI8(b + 102);
    z.fine_tune = ReadI8(b + 103);
    z.gain = ReadF32LE(b + 104, 0.0f, kGainMax, 1.0f);
    z.pan = ReadF32LE(b + 108, 0.0f, 1.0f, 0.5f);
    z.start_frame = ReadU32LE(b + 112);
    z.end_frame = ReadU32LE(b + 116);
    z.loop_start = ReadU32LE(b + 120);
    z.loop_end = ReadU32LE(b + 124);
    z.loop_mode = b[128];
    z.choke_group = b[129];
    z.output_bus = b[130];
    z.flags = b[131];
    z.cutoff_hz = ReadF32LE(b + 132, 0.0f, kCutoffMax, 20000.0f);
    z.attack_s = ReadF32LE(b + 136, 0.0f, kTimeMax, 0.001f);
    z.decay_s = ReadF32LE(b + 140, 0.0f, kTimeMax, 0.05f);
    z.sustain = ReadF32LE(b + 144, 0.0f, 1.0f, 0.8f);
    z.release_s = ReadF32LE(b + 148, 0.0f, kTimeMax, 0.1f);
}

inline void EncodeOscHeader(const Oscillator& osc, uint8_t* b) {
    WriteU16LE(b + 0, static_cast<uint16_t>(kOscHeaderWireSize));
    WriteU16LE(b + 2, static_cast<uint16_t>(kZoneWireSize));
    b[4] = osc.zone_count;
    b[5] = static_cast<uint8_t>(osc.type);
    WriteF32LE(b + 6, osc.level);
    WriteF32LE(b + 10, osc.pan);
    WriteI8(b + 14, osc.coarse_tune);
    WriteI8(b + 15, osc.fine_tune);
    b[16] = osc.keytrack;
}

inline void DecodeOscHeader(const uint8_t* b, Oscillator& osc) {
    const uint8_t type = b[5];
    osc.type = (type <= static_cast<uint8_t>(OscType::Wavetable)) ? static_cast<OscType>(type)
                                                                  : OscType::Off;
    osc.level = ReadF32LE(b + 6, 0.0f, kGainMax, 1.0f);
    osc.pan = ReadF32LE(b + 10, 0.0f, 1.0f, 0.5f);
    osc.coarse_tune = ReadI8(b + 14);
    osc.fine_tune = ReadI8(b + 15);
    osc.keytrack = b[16] ? 1 : 0;
}

inline void EncodeFilt(const Filter& f, uint8_t* b) {
    b[0] = static_cast<uint8_t>(f.type);
    WriteF32LE(b + 1, f.cutoff_hz);
    WriteF32LE(b + 5, f.resonance);
    WriteF32LE(b + 9, f.keytrack);
    WriteF32LE(b + 13, f.env2_amount);
}

inline void DecodeFilt(const uint8_t* b, Filter& f) {
    f.type = (b[0] <= static_cast<uint8_t>(FilterType::SvfNotch)) ? static_cast<FilterType>(b[0])
                                                                  : FilterType::SvfLp;
    f.cutoff_hz = ReadF32LE(b + 1, 0.0f, kCutoffMax, 20000.0f);
    f.resonance = ReadF32LE(b + 5, 0.0f, 1.0f, 0.0f);
    f.keytrack = ReadF32LE(b + 9, -kAmountMax, kAmountMax, 0.0f);
    f.env2_amount = ReadF32LE(b + 13, -kAmountMax, kAmountMax, 0.0f);
}

inline void EncodeEnv(const Adsr& e, uint8_t* b) {
    WriteF32LE(b + 0, e.attack_s);
    WriteF32LE(b + 4, e.decay_s);
    WriteF32LE(b + 8, e.sustain);
    WriteF32LE(b + 12, e.release_s);
}

inline void DecodeEnv(const uint8_t* b, Adsr& e) {
    e.attack_s = ReadF32LE(b + 0, 0.0f, kTimeMax, 0.001f);
    e.decay_s = ReadF32LE(b + 4, 0.0f, kTimeMax, 0.05f);
    e.sustain = ReadF32LE(b + 8, 0.0f, 1.0f, 0.8f);
    e.release_s = ReadF32LE(b + 12, 0.0f, kTimeMax, 0.1f);
}

inline void EncodeLfo(const LfoParams& l, uint8_t* b) {
    b[0] = l.wave;
    WriteF32LE(b + 1, l.rate_hz);
    b[5] = l.sync_div;
    WriteF32LE(b + 6, l.delay_s);
    WriteF32LE(b + 10, l.fade_s);
    b[14] = l.retrigger;
}

inline void DecodeLfo(const uint8_t* b, LfoParams& l) {
    l.wave = b[0];
    l.rate_hz = ReadF32LE(b + 1, 0.0f, kRateMax, 1.0f);
    l.sync_div = b[5];
    l.delay_s = ReadF32LE(b + 6, 0.0f, kTimeMax, 0.0f);
    l.fade_s = ReadF32LE(b + 10, 0.0f, kTimeMax, 0.0f);
    l.retrigger = b[14] ? 1 : 0;
}

inline void EncodeModSlot(const ModSlot& m, uint8_t* b) {
    b[0] = m.source;
    b[1] = m.dest;
    WriteI16LE(b + 2, m.depth);
    b[4] = m.curve;
    b[5] = m.flags;
}

inline void DecodeModSlot(const uint8_t* b, ModSlot& m) {
    m.source = b[0];
    m.dest = b[1];
    m.depth = ReadI16LE(b + 2);
    m.curve = b[4];
    m.flags = b[5];
}

inline uint32_t OscChunkSize(const Oscillator& osc) {
    return kOscHeaderWireSize + static_cast<uint32_t>(osc.zone_count) * kZoneWireSize;
}

// The WXCF header's advisory total_len. Cheap to compute exactly here - every
// chunk's size is known before a byte is written - and an accurate value lets
// a later tool sanity-check a file's length without parsing it.
inline uint32_t TotalFileSize(const InstrumentFile& doc) {
    const uint32_t chunk_hdr = static_cast<uint32_t>(Wxcf::kChunkHeaderSize);
    uint32_t n = static_cast<uint32_t>(Wxcf::kHeaderSize);
    n += chunk_hdr + kHeadWireSize;
    for (uint8_t i = 0; i < kNumOscillators; ++i)
        n += chunk_hdr + OscChunkSize(doc.osc[i]);
    n += chunk_hdr + kFiltWireSize;
    n += chunk_hdr + kAmpWireSize;
    n += (chunk_hdr + kEnvWireSize) * kNumEnvelopes;
    n += (chunk_hdr + kLfoWireSize) * kNumVoiceLfos;
    n += chunk_hdr + kModmHeaderWireSize + kModSlotWireSize * kMaxModSlots;
    return n;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

// Writes a complete .wxi byte stream through `io`. Every chunk is emitted
// every time, including the ones whose values the engine cannot yet produce:
// a file carrying a default FILT is a file a later build can edit in place,
// whereas an absent one would have to be invented on load anyway.
//
// Zones beyond `zone_count` are not written, and `zone_count` is clamped to
// what this build can hold, so a caller that leaves the array partly filled
// cannot emit uninitialised paths.
inline Result Write(Wxcf::IoContext io, const InstrumentFile& doc) {
    Wxcf::Writer w(io);
    uint8_t buf[kScratchBytes];

    if (w.WriteHeader(kFileType, kFileVersion, detail::TotalFileSize(doc)) != Wxcf::Result::Ok)
        return Result::IoError;

    detail::EncodeHead(doc, buf);
    if (w.WriteChunk(kChunkHead, kChunkVersion, buf, kHeadWireSize) != Wxcf::Result::Ok)
        return Result::IoError;

    for (uint8_t i = 0; i < kNumOscillators; ++i) {
        const Oscillator& osc = doc.osc[i];
        const uint8_t count = osc.zone_count < kMaxZonesPerOsc ? osc.zone_count : kMaxZonesPerOsc;
        const uint16_t id = static_cast<uint16_t>(i == 0 ? kChunkOsc1 : kChunkOsc2);
        // Streamed rather than buffered: 32 zone records is ~5 KB, far past
        // what belongs on the Daisy's stack (wxcf.hpp's BeginChunk/WriteData).
        if (w.BeginChunk(id,
                         kChunkVersion,
                         kOscHeaderWireSize + static_cast<uint32_t>(count) * kZoneWireSize) !=
            Wxcf::Result::Ok)
            return Result::IoError;
        detail::EncodeOscHeader(osc, buf);
        buf[4] = count;
        if (w.WriteData(buf, kOscHeaderWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
        for (uint8_t z = 0; z < count; ++z) {
            detail::EncodeZone(osc.zones[z], buf);
            if (w.WriteData(buf, kZoneWireSize) != Wxcf::Result::Ok)
                return Result::IoError;
        }
    }

    detail::EncodeFilt(doc.filter, buf);
    if (w.WriteChunk(kChunkFilt, kChunkVersion, buf, kFiltWireSize) != Wxcf::Result::Ok)
        return Result::IoError;

    buf[0] = doc.amp.velocity_curve;
    if (w.WriteChunk(kChunkAmp, kChunkVersion, buf, kAmpWireSize) != Wxcf::Result::Ok)
        return Result::IoError;

    for (uint8_t i = 0; i < kNumEnvelopes; ++i) {
        detail::EncodeEnv(doc.env[i], buf);
        const uint16_t id = static_cast<uint16_t>(kChunkEnv1 + i);
        if (w.WriteChunk(id, kChunkVersion, buf, kEnvWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
    }

    for (uint8_t i = 0; i < kNumVoiceLfos; ++i) {
        detail::EncodeLfo(doc.lfo[i], buf);
        const uint16_t id = static_cast<uint16_t>(kChunkLfo1 + i);
        if (w.WriteChunk(id, kChunkVersion, buf, kLfoWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
    }

    buf[0] = kMaxModSlots;
    buf[1] = static_cast<uint8_t>(kModSlotWireSize);
    for (uint8_t i = 0; i < kMaxModSlots; ++i)
        detail::EncodeModSlot(doc.mod_slots[i], buf + kModmHeaderWireSize + i * kModSlotWireSize);
    if (w.WriteChunk(kChunkModm,
                     kChunkVersion,
                     buf,
                     kModmHeaderWireSize + kModSlotWireSize * kMaxModSlots) != Wxcf::Result::Ok)
        return Result::IoError;

    return Result::Ok;
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

namespace detail {

// Reads the prefix of a chunk payload this build understands and discards the
// rest, which is how a later writer's extra fields stay harmless. A payload
// shorter than `want` is corruption - see the file comment.
inline Result ReadFixedPayload(Wxcf::Reader& r, uint32_t payload_len, uint8_t* buf, uint32_t want) {
    if (payload_len < want)
        return Result::BadChunk;
    if (r.ReadPayload(buf, want) != Wxcf::Result::Ok)
        return Result::IoError;
    if (r.SkipPayload(payload_len - want) != Wxcf::Result::Ok)
        return Result::IoError;
    return Result::Ok;
}

inline Result ReadOscChunk(Wxcf::Reader& r, uint32_t payload_len, Oscillator& out) {
    uint8_t buf[kScratchBytes];
    // header_len and zone_stride come first precisely so they can be read
    // before anything that depends on them.
    if (payload_len < 4)
        return Result::BadChunk;
    if (r.ReadPayload(buf, 4) != Wxcf::Result::Ok)
        return Result::IoError;
    const uint32_t header_len = ReadU16LE(buf + 0);
    const uint32_t zone_stride = ReadU16LE(buf + 2);
    if (header_len < kOscHeaderWireSize || header_len > payload_len)
        return Result::BadChunk;
    if (r.ReadPayload(buf + 4, kOscHeaderWireSize - 4) != Wxcf::Result::Ok)
        return Result::IoError;
    if (r.SkipPayload(header_len - kOscHeaderWireSize) != Wxcf::Result::Ok)
        return Result::IoError;

    const uint32_t zone_count = buf[4];
    const uint32_t body = payload_len - header_len;
    // A stride narrower than ours could only come from a writer that dropped
    // a Zone field, which the wire-stability rule forbids; treat it as
    // corruption rather than decoding garbage into the trailing fields.
    if (zone_count > 0 && zone_stride < kZoneWireSize)
        return Result::BadChunk;
    if (zone_count * zone_stride != body)
        return Result::BadChunk;
    if (zone_count > kMaxZonesPerOsc)
        return Result::TooManyZones;

    DecodeOscHeader(buf, out);
    out.zone_count = static_cast<uint8_t>(zone_count);
    for (uint32_t i = 0; i < zone_count; ++i) {
        if (r.ReadPayload(buf, kZoneWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
        if (r.SkipPayload(zone_stride - kZoneWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
        // `index` names a slot in the engine's zone array. A value this build
        // cannot address is rejected rather than clamped: clamping would
        // silently collapse two zones onto one slot, and the bridge that
        // consumes it would otherwise index out of bounds.
        if (buf[0] >= kMaxZonesPerOsc)
            return Result::BadChunk;
        DecodeZone(buf, out.zones[i]);
    }
    return Result::Ok;
}

inline Result ReadModmChunk(Wxcf::Reader& r, uint32_t payload_len, InstrumentFile& doc) {
    uint8_t buf[kScratchBytes];
    if (payload_len < kModmHeaderWireSize)
        return Result::BadChunk;
    if (r.ReadPayload(buf, kModmHeaderWireSize) != Wxcf::Result::Ok)
        return Result::IoError;
    const uint32_t slot_count = buf[0];
    const uint32_t slot_stride = buf[1];
    if (slot_count > 0 && slot_stride < kModSlotWireSize)
        return Result::BadChunk;
    if (slot_count * slot_stride != payload_len - kModmHeaderWireSize)
        return Result::BadChunk;
    for (uint32_t i = 0; i < slot_count; ++i) {
        if (r.ReadPayload(buf, kModSlotWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
        if (r.SkipPayload(slot_stride - kModSlotWireSize) != Wxcf::Result::Ok)
            return Result::IoError;
        // Unlike a Zone index, a mod row past this build's matrix is simply
        // dropped: rows are independent, so the ones we can hold still apply.
        if (i < kMaxModSlots)
            DecodeModSlot(buf, doc.mod_slots[i]);
    }
    return Result::Ok;
}

}  // namespace detail

// Parses a .wxi byte stream from `io` into `out`, which is fully reset first,
// so every field a file omits keeps its documented default.
//
// Chunks may appear in any order and any may be absent - except HEAD, whose
// absence means the stream is not an Instrument this build can use (and is
// the one cheap check that catches a file truncated to its container header).
inline Result Read(Wxcf::IoContext io, InstrumentFile& out) {
    out = InstrumentFile{};
    Wxcf::Reader r(io);

    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    const Wxcf::Result hr = r.ReadHeader(file_type, file_version, total_len);
    if (hr == Wxcf::Result::BadMagic)
        return Result::BadMagic;
    if (hr != Wxcf::Result::Ok)
        return Result::IoError;
    if (file_type != kFileType)
        return Result::BadFileType;
    if (Wxcf::VersionMajor(file_version) != Wxcf::VersionMajor(kFileVersion))
        return Result::BadVersion;

    uint8_t buf[kScratchBytes];
    bool saw_head = false;
    Wxcf::ChunkHeader ch;
    // NextChunkHeader() failing at a chunk boundary is EOF, which is how a
    // well-formed file ends - the container has no end marker (wxcf.hpp).
    while (r.NextChunkHeader(ch) == Wxcf::Result::Ok) {
        Result res = Result::Ok;
        switch (ch.chunk_id) {
            case kChunkHead:
                res = detail::ReadFixedPayload(r, ch.payload_len, buf, kHeadWireSize);
                if (res == Result::Ok) {
                    detail::DecodeHead(buf, out);
                    saw_head = true;
                }
                break;
            case kChunkOsc1:
                res = detail::ReadOscChunk(r, ch.payload_len, out.osc[0]);
                break;
            case kChunkOsc2:
                res = detail::ReadOscChunk(r, ch.payload_len, out.osc[1]);
                break;
            case kChunkFilt:
                res = detail::ReadFixedPayload(r, ch.payload_len, buf, kFiltWireSize);
                if (res == Result::Ok)
                    detail::DecodeFilt(buf, out.filter);
                break;
            case kChunkAmp:
                res = detail::ReadFixedPayload(r, ch.payload_len, buf, kAmpWireSize);
                if (res == Result::Ok)
                    out.amp.velocity_curve = buf[0];
                break;
            case kChunkEnv1:
            case kChunkEnv2:
            case kChunkEnv3:
                res = detail::ReadFixedPayload(r, ch.payload_len, buf, kEnvWireSize);
                if (res == Result::Ok)
                    detail::DecodeEnv(buf, out.env[ch.chunk_id - kChunkEnv1]);
                break;
            case kChunkLfo1:
            case kChunkLfo2:
                res = detail::ReadFixedPayload(r, ch.payload_len, buf, kLfoWireSize);
                if (res == Result::Ok)
                    detail::DecodeLfo(buf, out.lfo[ch.chunk_id - kChunkLfo1]);
                break;
            case kChunkModm:
                res = detail::ReadModmChunk(r, ch.payload_len, out);
                break;
            default:
                // Includes the reserved FXCH: forward compatibility is this
                // one line (instrument-model.md §5).
                if (r.SkipPayload(ch.payload_len) != Wxcf::Result::Ok)
                    return Result::IoError;
                break;
        }
        if (res != Result::Ok)
            return res;
    }

    return saw_head ? Result::Ok : Result::BadChunk;
}

}  // namespace Wxi
}  // namespace WaveX
