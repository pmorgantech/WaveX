#pragma once

// Project file codec. Foreground-only; explicit little-endian records.
// The caller owns private transaction scratch and publishes only after Done.
#include "sequencer/project_data.hpp"
#include "wxcf/pattern_file.hpp"
#include <cmath>
#include <cstring>

namespace WaveX {
namespace ProjectFile {
using PatternFile::Result;
using namespace Sequencer;
constexpr uint16_t kFileType = 6;
constexpr uint16_t kVersion = 0x0101;
constexpr uint32_t kMaxFileBytes = 4 * 1024 * 1024;
constexpr uint32_t kHeadBytes = 36, kTrackBytes = 272, kPatternPrefixBytes = 44;
constexpr uint32_t kPatternBytes =
    kPatternPrefixBytes + kMaxTracks * kMaxSteps * PatternFile::kStepBytes;
constexpr uint32_t kSongPrefixBytes = 28;
constexpr uint32_t kSampleEditBytes = 36;
constexpr uint32_t kSampleBytes = Protocol::BROWSE_PATH_MAX + kSampleEditBytes;

namespace detail {
using Wxcf::detail::ReadU16LE;
using Wxcf::detail::WriteU16LE;
inline void Float(uint8_t* b, float v) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "32-bit file floats");
    std::memcpy(&bits, &v, sizeof(bits));
    Wxcf::detail::WriteU32LE(b, bits);
}
inline float Float(const uint8_t* b) {
    const uint32_t bits = Wxcf::detail::ReadU32LE(b);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
inline bool Path(const char* path) {
    // An empty path means no Instrument/Bank. Never accept truncated or
    // control-character-containing paths as a different file's identity.
    for (size_t i = 0; i < Protocol::BROWSE_PATH_MAX; ++i) {
        const auto c = static_cast<unsigned char>(path[i]);
        if (!c)
            return true;
        if (c < 32 || c == 127)
            return false;
    }
    return false;
}
inline bool Tempo(uint16_t bpm) {
    return bpm >= 2000 && bpm <= 30000;
}
inline bool Gain(float gain) {
    return std::isfinite(gain) && gain >= 0 && gain <= 64;
}
inline bool Head(const Project& p) {
    return PatternFile::ValidName(p.name) && Tempo(p.tempo_bpm_x100) &&
           p.active_pattern < kMaxPatterns &&
           (p.selected_song == kNoSong || p.selected_song < kMaxSongs) &&
           p.clock_source <= Protocol::SEQ_CLOCK_MIDI &&
           p.input_mode <= Protocol::SEQ_INPUT_LIVE_ERASE && Gain(p.master_gain);
}
inline bool Track(const ProjectTrack& t) {
    return Path(t.instrument_path) && Protocol::TrackMidiInValid(t.midi_in) &&
           t.poly_limit <= 128 && Gain(t.mix.gain) && std::isfinite(t.mix.pan_offset) &&
           t.mix.pan_offset >= -1 && t.mix.pan_offset <= 1;
}
inline bool Sample(const ProjectSample& s) {
    const auto end = s.end_frame ? s.end_frame : s.total_frames;
    const auto loop_end = s.loop_end ? s.loop_end : end;
    return s.path[0] && Path(s.path) && s.sample_rate && s.total_frames &&
           (s.channels == 1 || s.channels == 2) && s.bits_per_sample == 16 && s.start_frame < end &&
           end <= s.total_frames && s.loop_start >= s.start_frame && s.loop_start < loop_end &&
           loop_end <= end && s.gain_db_x10 >= -240 && s.gain_db_x10 <= 120 &&
           s.channel_mode <= Protocol::SAMPLE_CH_MONO_SUM;
}
inline bool UniqueSample(const Project& p, uint16_t index) {
    for (uint16_t i = 0; i < index; ++i)
        if (std::strcmp(p.samples[i].path, p.samples[index].path) == 0)
            return false;
    return true;
}
inline void EncodeSampleEdits(uint8_t* b, const ProjectSample& s) {
    Wxcf::detail::WriteU32LE(b, s.sample_rate);
    Wxcf::detail::WriteU32LE(b + 4, s.total_frames);
    Wxcf::detail::WriteU32LE(b + 8, s.start_frame);
    Wxcf::detail::WriteU32LE(b + 12, s.end_frame);
    Wxcf::detail::WriteU32LE(b + 16, s.loop_start);
    Wxcf::detail::WriteU32LE(b + 20, s.loop_end);
    WriteU16LE(b + 24, static_cast<uint16_t>(s.gain_db_x10));
    WriteU16LE(b + 26, s.fade_in_ms);
    WriteU16LE(b + 28, s.fade_out_ms);
    b[30] = s.channels;
    b[31] = s.bits_per_sample;
    b[32] = s.loop_enabled;
    b[33] = s.channel_mode;
}
inline bool DecodeSampleEdits(const uint8_t* b, ProjectSample& s) {
    s.sample_rate = Wxcf::detail::ReadU32LE(b);
    s.total_frames = Wxcf::detail::ReadU32LE(b + 4);
    s.start_frame = Wxcf::detail::ReadU32LE(b + 8);
    s.end_frame = Wxcf::detail::ReadU32LE(b + 12);
    s.loop_start = Wxcf::detail::ReadU32LE(b + 16);
    s.loop_end = Wxcf::detail::ReadU32LE(b + 20);
    const auto gain = ReadU16LE(b + 24);
    s.gain_db_x10 = static_cast<int16_t>(gain < 32768 ? gain : static_cast<int32_t>(gain) - 65536);
    s.fade_in_ms = ReadU16LE(b + 26);
    s.fade_out_ms = ReadU16LE(b + 28);
    s.channels = b[30];
    s.bits_per_sample = b[31];
    s.loop_enabled = b[32] != 0;
    s.channel_mode = b[33];
    return b[32] <= 1 && !b[34] && !b[35] && Sample(s);
}
inline bool Links(const Project& p) {
    if (!p.patterns[p.active_pattern].used ||
        (p.selected_song != kNoSong && !p.songs[p.selected_song].used))
        return false;
    for (const auto& song: p.songs) {
        if (!song.used)
            continue;
        if (!song.length || song.length > kMaxSongEntries || !PatternFile::ValidName(song.name) ||
            !Tempo(song.tempo_bpm_x100))
            return false;
        for (uint16_t i = 0; i < song.length; ++i)
            if (!song.entries[i].repeats || song.entries[i].pattern >= kMaxPatterns ||
                !p.patterns[song.entries[i].pattern].used)
                return false;
    }
    return true;
}
}  // namespace detail

class Encoder {
   public:
    Encoder(Wxcf::IoContext io, const Project& project) : writer_(io), p_(project) {}
    Result Advance() {
        if (result_ != Result::More)
            return result_;
        uint8_t b[kTrackBytes]{};
        if (phase_ == 0) {
            if (!detail::Head(p_) || !detail::Path(p_.bank_path) || !detail::Links(p_) ||
                p_.sample_count > kMaxProjectSamples)
                return result_ = Result::Invalid;
            std::memcpy(b, p_.name, sizeof(p_.name));
            detail::WriteU16LE(b + 24, p_.tempo_bpm_x100);
            b[26] = p_.active_pattern;
            b[27] = p_.selected_song;
            b[28] = p_.clock_source;
            b[29] = p_.input_mode;
            b[30] = p_.quantize;
            detail::Float(b + 32, p_.master_gain);
            if (writer_.WriteHeader(kFileType, kVersion, 0) != Wxcf::Result::Ok ||
                !Chunk(1, b, kHeadBytes))
                return result_ = Result::IoError;
            ++phase_;
            return result_;
        }
        if (phase_ == 1) {
            if (!Chunk(2, p_.bank_path, Protocol::BROWSE_PATH_MAX))
                return result_ = Result::IoError;
            ++phase_;
            return result_;
        }
        if (phase_ == 2) {
            const auto& t = p_.tracks[index_];
            if (!detail::Track(t))
                return result_ = Result::Invalid;
            std::memcpy(b, t.instrument_path, sizeof(t.instrument_path));
            b[256] = t.midi_in;
            b[257] = t.poly_limit;
            b[258] = t.priority;
            b[259] = t.program_change;
            detail::Float(b + 260, t.mix.gain);
            detail::Float(b + 264, t.mix.pan_offset);
            b[268] = t.mix.mute;
            if (!Chunk(static_cast<uint16_t>(0x100 + index_), b, kTrackBytes))
                return result_ = Result::IoError;
            if (++index_ == kMaxTracks) {
                index_ = 0;
                ++phase_;
            }
            return result_;
        }
        if (phase_ == 3) {
            if (!record_) {
                while (index_ < kMaxPatterns && !p_.patterns[index_].used)
                    ++index_;
                if (index_ == kMaxPatterns) {
                    index_ = 0;
                    ++phase_;
                    return result_;
                }
                const auto& slot = p_.patterns[index_];
                const auto& pattern = slot.pattern;
                if (!PatternFile::ValidName(slot.name) || !pattern.length ||
                    pattern.length > kMaxSteps || static_cast<uint8_t>(pattern.scale) > 5 ||
                    pattern.swing < 50 || pattern.swing > 75)
                    return result_ = Result::Invalid;
                std::memcpy(b, slot.name, sizeof(slot.name));
                b[24] = pattern.length;
                b[25] = static_cast<uint8_t>(pattern.scale);
                b[26] = pattern.swing;
                for (uint8_t t = 0; t < kMaxTracks; ++t)
                    b[28 + t] = pattern.tracks[t].enabled;
                if (writer_.BeginChunk(static_cast<uint16_t>(0x200 + index_),
                                       kVersion,
                                       kPatternBytes) != Wxcf::Result::Ok ||
                    writer_.WriteData(b, kPatternPrefixBytes) != Wxcf::Result::Ok)
                    return result_ = Result::IoError;
                record_ = 1;
                return result_;
            }
            const auto& step =
                p_.patterns[index_].pattern.tracks[(record_ - 1) / kMaxSteps].steps[(record_ - 1) %
                                                                                    kMaxSteps];
            if (!PatternFile::ValidStep(step))
                return result_ = Result::Invalid;
            PatternFile::EncodeStep(step, b);
            if (writer_.WriteData(b, PatternFile::kStepBytes) != Wxcf::Result::Ok)
                return result_ = Result::IoError;
            if (++record_ > kMaxTracks * kMaxSteps) {
                record_ = 0;
                ++index_;
            }
            return result_;
        }
        if (phase_ == 5) {
            detail::WriteU16LE(b, p_.sample_count);
            if (!Chunk(3, b, 2))
                return result_ = Result::IoError;
            ++phase_;
            return result_;
        }
        if (phase_ == 6) {
            if (index_ == p_.sample_count)
                return result_ = Result::Done;
            const auto& sample = p_.samples[index_];
            if (!record_) {
                if (!detail::Sample(sample) || !detail::UniqueSample(p_, index_))
                    return result_ = Result::Invalid;
                if (writer_.BeginChunk(static_cast<uint16_t>(0x1000 + index_),
                                       kVersion,
                                       kSampleBytes) != Wxcf::Result::Ok ||
                    writer_.WriteData(sample.path, sizeof(sample.path)) != Wxcf::Result::Ok)
                    return result_ = Result::IoError;
                record_ = 1;
            } else {
                detail::EncodeSampleEdits(b, sample);
                if (writer_.WriteData(b, kSampleEditBytes) != Wxcf::Result::Ok)
                    return result_ = Result::IoError;
                record_ = 0;
                ++index_;
            }
            return result_;
        }
        while (index_ < kMaxSongs && !p_.songs[index_].used)
            ++index_;
        if (index_ == kMaxSongs) {
            index_ = 0;
            ++phase_;
            return result_;
        }
        const auto& song = p_.songs[index_];
        if (!record_) {
            std::memcpy(b, song.name, sizeof(song.name));
            detail::WriteU16LE(b + 24, song.tempo_bpm_x100);
            detail::WriteU16LE(b + 26, song.length);
            if (writer_.BeginChunk(static_cast<uint16_t>(0x300 + index_),
                                   kVersion,
                                   kSongPrefixBytes + song.length * 2u) != Wxcf::Result::Ok ||
                writer_.WriteData(b, kSongPrefixBytes) != Wxcf::Result::Ok)
                return result_ = Result::IoError;
            record_ = 1;
            return result_;
        }
        b[0] = song.entries[record_ - 1].pattern;
        b[1] = song.entries[record_ - 1].repeats;
        if (writer_.WriteData(b, 2) != Wxcf::Result::Ok)
            return result_ = Result::IoError;
        if (++record_ > song.length) {
            record_ = 0;
            ++index_;
        }
        return result_;
    }

   private:
    bool Chunk(uint16_t id, const void* bytes, uint32_t size) {
        return writer_.WriteChunk(id, kVersion, bytes, size) == Wxcf::Result::Ok;
    }
    Wxcf::Writer writer_;
    const Project& p_;
    uint16_t index_ = 0, record_ = 0;
    uint8_t phase_ = 0;
    Result result_ = Result::More;
};

class Decoder {
   public:
    Decoder(Wxcf::IoContext io, Project& project) : reader_(io), p_(project) {
        p_.sample_count = 0;
        for (auto& pattern: p_.patterns)
            pattern.used = false;
        for (auto& song: p_.songs)
            song.used = false;
    }
    Result Advance() {
        if (result_ != Result::More)
            return result_;
        if (!started_) {
            uint16_t type = 0, version = 0;
            uint32_t length = 0;
            if (reader_.ReadHeader(type, version, length) != Wxcf::Result::Ok ||
                type != kFileType || Wxcf::VersionMajor(version) != 1 || length > kMaxFileBytes)
                return Fail(Result::Invalid);
            needs_samples_ = version >= 0x0101;
            started_ = true;
            return result_;
        }
        if (skip_) {
            const uint32_t n = skip_ > 128 ? 128 : skip_;
            if (reader_.SkipPayload(n) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            skip_ -= n;
            return result_;
        }
        uint8_t b[kTrackBytes]{};
        if (sample_pending_) {
            auto& sample = p_.samples[samples_read_];
            if (!Read(b, kSampleEditBytes))
                return result_;
            if (!detail::DecodeSampleEdits(b, sample) || !detail::UniqueSample(p_, samples_read_))
                return Fail(Result::Invalid);
            ++samples_read_;
            sample_pending_ = false;
            return result_;
        }
        if (record_) {
            if (song_) {
                if (!Read(b, 2))
                    return result_;
                auto& entry = p_.songs[index_].entries[record_ - 1];
                entry.pattern = b[0];
                entry.repeats = b[1];
                if (entry.pattern >= kMaxPatterns || !entry.repeats)
                    return Fail(Result::Invalid);
                if (++record_ > p_.songs[index_].length)
                    record_ = 0;
            } else {
                if (!Read(b, PatternFile::kStepBytes))
                    return result_;
                auto& step = p_.patterns[index_]
                                 .pattern.tracks[(record_ - 1) / kMaxSteps]
                                 .steps[(record_ - 1) % kMaxSteps];
                if (!PatternFile::DecodeStep(b, step))
                    return Fail(Result::Invalid);
                if (++record_ > kMaxTracks * kMaxSteps)
                    record_ = 0;
            }
            return result_;
        }
        Wxcf::ChunkHeader h;
        const auto r = reader_.NextChunkHeader(h);
        if (r == Wxcf::Result::EndOfFile)
            return result_ = head_ && bank_ && tracks_ == 0xffff && detail::Head(p_) &&
                                     detail::Links(p_) && (!needs_samples_ || samples_head_) &&
                                     samples_read_ == p_.sample_count
                                 ? Result::Done
                                 : Result::Invalid;
        if (r != Wxcf::Result::Ok)
            return Fail(Result::IoError);
        if (h.payload_len > kMaxFileBytes - 8 || consumed_ > kMaxFileBytes - 8 - h.payload_len)
            return Fail(Result::Invalid);
        consumed_ += 8 + h.payload_len;
        const bool major = Wxcf::VersionMajor(h.chunk_version) == 1;
        if (h.chunk_id == 1) {
            if (head_ || !major || h.payload_len != kHeadBytes)
                return Fail(Result::Invalid);
            if (!Read(b, kHeadBytes))
                return result_;
            std::memcpy(p_.name, b, sizeof(p_.name));
            p_.tempo_bpm_x100 = detail::ReadU16LE(b + 24);
            p_.active_pattern = b[26];
            p_.selected_song = b[27];
            p_.clock_source = b[28];
            p_.input_mode = b[29];
            p_.quantize = b[30] != 0;
            p_.master_gain = detail::Float(b + 32);
            if (b[30] > 1 || b[31] || !detail::Head(p_))
                return Fail(Result::Invalid);
            head_ = true;
        } else if (h.chunk_id == 2) {
            if (bank_ || !major || h.payload_len != Protocol::BROWSE_PATH_MAX)
                return Fail(Result::Invalid);
            if (!Read(p_.bank_path, Protocol::BROWSE_PATH_MAX))
                return result_;
            if (!detail::Path(p_.bank_path))
                return Fail(Result::Invalid);
            bank_ = true;
        } else if (h.chunk_id == 3) {
            if (samples_head_ || !major || h.payload_len != 2)
                return Fail(Result::Invalid);
            if (!Read(b, 2))
                return result_;
            p_.sample_count = detail::ReadU16LE(b);
            if (p_.sample_count > kMaxProjectSamples)
                return Fail(Result::Invalid);
            samples_head_ = true;
        } else if (h.chunk_id >= 0x1000 && h.chunk_id < 0x1000 + kMaxProjectSamples) {
            // Ordered, contiguous records make completeness and duplicate detection
            // independent of a large MCU-side bitmap. Paths remain the identity.
            if (!samples_head_ || !major || h.payload_len != kSampleBytes ||
                h.chunk_id != 0x1000 + samples_read_ || samples_read_ >= p_.sample_count)
                return Fail(Result::Invalid);
            if (!Read(p_.samples[samples_read_].path, Protocol::BROWSE_PATH_MAX))
                return result_;
            sample_pending_ = true;
        } else if (h.chunk_id >= 0x100 && h.chunk_id < 0x100 + kMaxTracks) {
            const uint8_t t = static_cast<uint8_t>(h.chunk_id - 0x100);
            if ((tracks_ & (1u << t)) || !major || h.payload_len != kTrackBytes)
                return Fail(Result::Invalid);
            if (!Read(b, kTrackBytes))
                return result_;
            auto& track = p_.tracks[t];
            std::memcpy(track.instrument_path, b, sizeof(track.instrument_path));
            track.midi_in = b[256];
            track.poly_limit = b[257];
            track.priority = b[258];
            track.program_change = b[259] != 0;
            track.mix.gain = detail::Float(b + 260);
            track.mix.pan_offset = detail::Float(b + 264);
            track.mix.mute = b[268] != 0;
            if (b[259] > 1 || b[268] > 1 || b[269] || b[270] || b[271] || !detail::Track(track))
                return Fail(Result::Invalid);
            tracks_ |= static_cast<uint16_t>(1u << t);
        } else if (h.chunk_id >= 0x200 && h.chunk_id < 0x200 + kMaxPatterns) {
            index_ = static_cast<uint8_t>(h.chunk_id - 0x200);
            auto& slot = p_.patterns[index_];
            auto& pattern = slot.pattern;
            if (slot.used || !major || h.payload_len != kPatternBytes)
                return Fail(Result::Invalid);
            if (!Read(b, kPatternPrefixBytes))
                return result_;
            std::memcpy(slot.name, b, sizeof(slot.name));
            pattern.length = b[24];
            pattern.scale = static_cast<StepScale>(b[25]);
            pattern.swing = b[26];
            if (!PatternFile::ValidName(slot.name) || !pattern.length ||
                pattern.length > kMaxSteps || b[25] > 5 || b[26] < 50 || b[26] > 75 || b[27])
                return Fail(Result::Invalid);
            for (uint8_t t = 0; t < kMaxTracks; ++t) {
                if (b[28 + t] > 1)
                    return Fail(Result::Invalid);
                pattern.tracks[t].enabled = b[28 + t] != 0;
            }
            slot.used = true;
            song_ = false;
            record_ = 1;
        } else if (h.chunk_id >= 0x300 && h.chunk_id < 0x300 + kMaxSongs) {
            index_ = static_cast<uint8_t>(h.chunk_id - 0x300);
            auto& song = p_.songs[index_];
            if (song.used || !major || h.payload_len < kSongPrefixBytes)
                return Fail(Result::Invalid);
            if (!Read(b, kSongPrefixBytes))
                return result_;
            std::memcpy(song.name, b, sizeof(song.name));
            song.tempo_bpm_x100 = detail::ReadU16LE(b + 24);
            song.length = detail::ReadU16LE(b + 26);
            if (!PatternFile::ValidName(song.name) || !detail::Tempo(song.tempo_bpm_x100) ||
                !song.length || song.length > kMaxSongEntries ||
                h.payload_len != kSongPrefixBytes + song.length * 2u)
                return Fail(Result::Invalid);
            song.used = true;
            song_ = true;
            record_ = 1;
        } else
            skip_ = h.payload_len;
        return result_;
    }

   private:
    Result Fail(Result r) { return result_ = r; }
    bool Read(void* bytes, uint32_t count) {
        if (reader_.ReadPayload(bytes, count) == Wxcf::Result::Ok)
            return true;
        Fail(Result::IoError);
        return false;
    }
    Wxcf::Reader reader_;
    Project& p_;
    uint32_t consumed_ = 12, skip_ = 0;
    uint16_t tracks_ = 0, record_ = 0, samples_read_ = 0;
    bool needs_samples_ = false, samples_head_ = false, sample_pending_ = false;
    uint8_t index_ = 0;
    bool started_ = false, head_ = false, bank_ = false, song_ = false;
    Result result_ = Result::More;
};
}  // namespace ProjectFile
}  // namespace WaveX
