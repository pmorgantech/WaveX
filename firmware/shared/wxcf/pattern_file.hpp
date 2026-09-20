#pragma once
// Incremental WXCF pattern codec. A document owns all sixteen rows, including
// hidden steps; tempo and Track instruments belong to the surrounding session.
// Each Advance performs at most one step record or 128 unknown payload bytes.
#include "sequencer/pattern_data.hpp"
#include "wxcf/wxcf.hpp"
#include <cstring>

namespace WaveX {
namespace PatternFile {
constexpr uint16_t kFileType = 5;
constexpr uint16_t kVersion = 0x0101;
constexpr uint32_t kMaxFileBytes = 65536;
constexpr size_t kNameBytes = 24;
constexpr uint32_t kLegacyStepBytes = 20;
constexpr uint32_t kStepBytes = 36;
constexpr uint32_t kTrackBytes = 1 + Sequencer::kMaxSteps * kStepBytes;
constexpr uint32_t kFileBytes = 12 + 8 + 28 + Sequencer::kMaxTracks * (8 + kTrackBytes);
enum class Result { More, Done, Invalid, IoError };

inline bool ValidName(const char* name) {
    if (!name || !name[0] || name[0] == ' ')
        return false;
    for (size_t i = 0; i < kNameBytes; ++i) {
        const char c = name[i];
        if (!c)
            return i > 0 && name[i - 1] != ' ';
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '_'))
            return false;
    }
    return false;
}
inline bool ValidStep(const Sequencer::Step& s) {
    if (s.note > 127 || s.velocity > 127 || s.probability > 100 ||
        s.retrig_count > Sequencer::kMaxRetrigCount)
        return false;
    for (const auto& n: s.notes)
        if (n.note > 127 || n.velocity > 127 || n.gate_ticks > 32767)
            return false;
    for (uint8_t i = 0; i < Sequencer::kMaxParamLocks; ++i) {
        const auto& a = s.param_locks[i];
        if (!a.param_id && a.value)
            return false;
        for (uint8_t j = 0; j < i; ++j)
            if (a.param_id && a.param_id == s.param_locks[j].param_id)
                return false;
    }
    return true;
}
inline bool ValidPattern(const Sequencer::Pattern& p) {
    if (!p.length || p.length > Sequencer::kMaxSteps || static_cast<uint8_t>(p.scale) > 5 ||
        p.swing < 50 || p.swing > 75)
        return false;
    for (const auto& t: p.tracks)
        for (const auto& s: t.steps)
            if (!ValidStep(s))
                return false;
    return true;
}
inline void EncodeStep(const Sequencer::Step& s, uint8_t* b) {
    b[0] = s.on;
    b[1] = s.note;
    b[2] = s.velocity;
    b[3] = s.probability;
    Wxcf::detail::WriteU16LE(b + 4, static_cast<uint16_t>(s.micro_offset));
    b[6] = s.retrig_count;
    b[7] = s.retrig_rate_ticks;
    for (uint8_t i = 0; i < 4; ++i) {
        b[8 + 3 * i] = s.param_locks[i].param_id;
        Wxcf::detail::WriteU16LE(b + 9 + 3 * i, s.param_locks[i].value);
    }
    for (uint8_t i = 0; i < 4; ++i) {
        b[20 + 4 * i] = s.notes[i].note;
        b[21 + 4 * i] = s.notes[i].velocity;
        Wxcf::detail::WriteU16LE(b + 22 + 4 * i, s.notes[i].gate_ticks);
    }
}
inline bool DecodeStep(const uint8_t* b, Sequencer::Step& s, bool melodic_format = true) {
    for (uint8_t i = 0; i < 4; ++i) {
        s.notes[i] = Sequencer::NoteLane{};
        if (melodic_format) {
            s.notes[i].note = b[20 + 4 * i];
            s.notes[i].velocity = b[21 + 4 * i];
            s.notes[i].gate_ticks = Wxcf::detail::ReadU16LE(b + 22 + 4 * i);
        }
    }
    if (b[0] > 1)
        return false;
    s.on = b[0];
    s.note = b[1];
    s.velocity = b[2];
    s.probability = b[3];
    const uint16_t micro = Wxcf::detail::ReadU16LE(b + 4);
    s.micro_offset = static_cast<int16_t>(micro < 32768 ? static_cast<int32_t>(micro)
                                                        : static_cast<int32_t>(micro) - 65536);
    s.retrig_count = b[6];
    s.retrig_rate_ticks = b[7];
    for (uint8_t i = 0; i < 4; ++i) {
        s.param_locks[i].param_id = b[8 + 3 * i];
        s.param_locks[i].value = Wxcf::detail::ReadU16LE(b + 9 + 3 * i);
    }
    return ValidStep(s);
}
class Encoder {
   public:
    Encoder(Wxcf::IoContext io, const Sequencer::Pattern& pattern, const char* name)
        : writer_(io), pattern_(pattern), name_(name) {}
    Result Advance() {
        if (result_ != Result::More)
            return result_;
        if (!started_) {
            if (!ValidName(name_) || !ValidPattern(pattern_))
                return result_ = Result::Invalid;
            uint8_t meta[28]{};
            std::memcpy(meta, name_, std::strlen(name_));
            meta[24] = pattern_.length;
            meta[25] = static_cast<uint8_t>(pattern_.scale);
            meta[26] = pattern_.swing;
            if (writer_.WriteHeader(kFileType, kVersion, kFileBytes) != Wxcf::Result::Ok ||
                writer_.WriteChunk(1, kVersion, meta, sizeof(meta)) != Wxcf::Result::Ok)
                return result_ = Result::IoError;
            started_ = true;
            return Result::More;
        }
        const auto& t = pattern_.tracks[track_];
        if (!step_) {
            uint8_t enabled = t.enabled | (t.melodic ? 2 : 0);
            if (writer_.BeginChunk(0x100 + track_, kVersion, kTrackBytes) != Wxcf::Result::Ok ||
                writer_.WriteData(&enabled, 1) != Wxcf::Result::Ok)
                return result_ = Result::IoError;
        }
        uint8_t data[kStepBytes];
        EncodeStep(t.steps[step_], data);
        if (writer_.WriteData(data, sizeof(data)) != Wxcf::Result::Ok)
            return result_ = Result::IoError;
        if (++step_ == Sequencer::kMaxSteps) {
            step_ = 0;
            if (++track_ == Sequencer::kMaxTracks)
                return result_ = Result::Done;
        }
        return Result::More;
    }

   private:
    Wxcf::Writer writer_;
    const Sequencer::Pattern& pattern_;
    const char* name_;
    uint8_t track_ = 0, step_ = 0;
    bool started_ = false;
    Result result_ = Result::More;
};
class Decoder {
   public:
    // The caller supplies private scratch storage. It must not expose this
    // partially decoded document to the scheduler before Done.
    Decoder(Wxcf::IoContext io, Sequencer::Pattern& pattern, char* name)
        : reader_(io), pattern_(pattern), name_(name) {}
    Result Advance() {
        if (result_ != Result::More)
            return result_;
        if (!started_) {
            uint16_t type = 0, version = 0;
            uint32_t length = 0;
            if (reader_.ReadHeader(type, version, length) != Wxcf::Result::Ok)
                return Fail(Result::Invalid);
            if (type != kFileType || Wxcf::VersionMajor(version) != 1 || length > kMaxFileBytes)
                return Fail(Result::Invalid);
            started_ = true;
            return Result::More;
        }
        if (skip_) {
            const uint32_t bytes = skip_ > 128 ? 128 : skip_;
            if (reader_.SkipPayload(bytes) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            skip_ -= bytes;
            return Result::More;
        }
        if (track_ < Sequencer::kMaxTracks) {
            uint8_t data[kStepBytes];
            if (reader_.ReadPayload(data, melodic_format_ ? kStepBytes : kLegacyStepBytes) !=
                Wxcf::Result::Ok)
                return Fail(Result::IoError);
            if (!DecodeStep(data, pattern_.tracks[track_].steps[step_], melodic_format_))
                return Fail(Result::Invalid);
            if (++step_ == Sequencer::kMaxSteps) {
                step_ = 0;
                track_ = Sequencer::kMaxTracks;
            }
            return Result::More;
        }
        Wxcf::ChunkHeader h;
        const auto read = reader_.NextChunkHeader(h);
        if (read == Wxcf::Result::EndOfFile)
            return result_ = meta_ && tracks_ == 0xffff ? Result::Done : Result::Invalid;
        if (read != Wxcf::Result::Ok)
            return Fail(Result::IoError);
        if (h.payload_len > kMaxFileBytes - 8 || consumed_ > kMaxFileBytes - 8 - h.payload_len)
            return Fail(Result::Invalid);
        consumed_ += 8 + h.payload_len;
        if (h.chunk_id == 1) {
            uint8_t b[28];
            if (meta_ || h.payload_len != sizeof(b) || Wxcf::VersionMajor(h.chunk_version) != 1)
                return Fail(Result::Invalid);
            if (reader_.ReadPayload(b, sizeof(b)) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            if (!ValidName(reinterpret_cast<char*>(b)) || !b[24] || b[24] > 64 || b[25] > 5 ||
                b[26] < 50 || b[26] > 75 || b[27])
                return Fail(Result::Invalid);
            std::memcpy(name_, b, kNameBytes);
            pattern_.length = b[24];
            pattern_.scale = static_cast<Sequencer::StepScale>(b[25]);
            pattern_.swing = b[26];
            meta_ = true;
        } else if (h.chunk_id >= 0x100 && h.chunk_id < 0x100 + Sequencer::kMaxTracks) {
            const uint8_t track = static_cast<uint8_t>(h.chunk_id - 0x100);
            melodic_format_ = h.chunk_version >= kVersion;
            const auto track_bytes =
                1 + Sequencer::kMaxSteps * (melodic_format_ ? kStepBytes : kLegacyStepBytes);
            if ((tracks_ & (1u << track)) || h.payload_len != track_bytes ||
                Wxcf::VersionMajor(h.chunk_version) != 1)
                return Fail(Result::Invalid);
            uint8_t enabled = 0;
            if (reader_.ReadPayload(&enabled, 1) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            if (enabled > (melodic_format_ ? 3 : 1))
                return Fail(Result::Invalid);
            tracks_ |= 1u << track;
            track_ = track;
            pattern_.tracks[track].enabled = (enabled & 1) != 0;
            pattern_.tracks[track].melodic = (enabled & 2) != 0;
        } else
            skip_ = h.payload_len;
        return Result::More;
    }

   private:
    Result Fail(Result r) { return result_ = r; }
    Wxcf::Reader reader_;
    Sequencer::Pattern& pattern_;
    char* name_;
    uint32_t consumed_ = 12, skip_ = 0;
    uint16_t tracks_ = 0;
    uint8_t track_ = Sequencer::kMaxTracks, step_ = 0;
    bool started_ = false, meta_ = false, melodic_format_ = false;
    Result result_ = Result::More;
};
}  // namespace PatternFile
}  // namespace WaveX
