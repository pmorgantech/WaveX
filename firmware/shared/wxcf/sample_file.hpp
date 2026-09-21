#pragma once

#include "spi_protocol/protocol.h"

#include "wxcf/wxcf.hpp"
#include <cstring>

namespace WaveX::SampleFile {
// Standalone, non-destructive edits. Runtime IDs, pointers, ownership flags and
// names never enter this file. The sidecar follows the complete WAV filename.
constexpr uint16_t kFileType = 8, kVersion = 0x0101;
constexpr uint32_t kPayloadBytes = 44, kFileBytes = 12 + 8 + kPayloadBytes;
constexpr uint32_t kMaxFileBytes = 4096;
struct Document {
    uint32_t file_bytes = 0, data_offset = 0;
    Protocol::SampleMetadata sample;
};
inline bool Valid(const Document& d) {
    const auto& m = d.sample;
    const uint32_t end = m.end_frame ? m.end_frame : m.total_frames;
    const uint32_t loop_end = m.loop_end ? m.loop_end : end;
    const uint64_t bytes = uint64_t(m.total_frames) * m.channels * (m.bits_per_sample / 8u);
    return m.sample_rate && m.total_frames && (m.channels == 1 || m.channels == 2) &&
           (m.bits_per_sample == 16 || m.bits_per_sample == 24) && d.data_offset <= d.file_bytes &&
           bytes <= d.file_bytes - d.data_offset && m.start_frame < end && end <= m.total_frames &&
           m.loop_start >= m.start_frame && m.loop_start < loop_end && loop_end <= end &&
           m.loop_enabled <= 1 && m.gain_db_x10 >= -240 && m.gain_db_x10 <= 120 &&
           m.channel_mode <= Protocol::SAMPLE_CH_MONO_SUM &&
           m.loop_crossfade_ms <= Protocol::kMaxLoopCrossfadeMs;
}
inline bool Matches(const Document& d, const Document& source) {
    return Valid(d) && d.file_bytes == source.file_bytes && d.data_offset == source.data_offset &&
           d.sample.sample_rate == source.sample.sample_rate &&
           d.sample.total_frames == source.sample.total_frames &&
           d.sample.channels == source.sample.channels &&
           d.sample.bits_per_sample == source.sample.bits_per_sample;
}
inline void Apply(const Document& d, Protocol::SampleMetadata& m) {
    const auto& s = d.sample;
    m.start_frame = s.start_frame;
    m.end_frame = s.end_frame;
    m.loop_start = s.loop_start;
    m.loop_end = s.loop_end;
    m.loop_enabled = s.loop_enabled;
    m.gain_db_x10 = s.gain_db_x10;
    m.fade_in_ms = s.fade_in_ms;
    m.fade_out_ms = s.fade_out_ms;
    if (m.channel_mode != s.channel_mode) {
        m.channel_mode = s.channel_mode;
        ++m.generation;
    }
    m.loop_crossfade_ms = s.loop_crossfade_ms;
    m.Resolve();
}
inline bool Write(Wxcf::IoContext io, const Document& d) {
    if (!io.write || !Valid(d))
        return false;
    uint8_t b[kPayloadBytes]{};
    const auto& m = d.sample;
    using namespace Wxcf::detail;
    WriteU32LE(b, d.file_bytes);
    WriteU32LE(b + 4, d.data_offset);
    WriteU32LE(b + 8, m.sample_rate);
    WriteU32LE(b + 12, m.total_frames);
    WriteU32LE(b + 16, m.start_frame);
    WriteU32LE(b + 20, m.end_frame);
    WriteU32LE(b + 24, m.loop_start);
    WriteU32LE(b + 28, m.loop_end);
    WriteU16LE(b + 32, static_cast<uint16_t>(m.gain_db_x10));
    WriteU16LE(b + 34, m.fade_in_ms);
    WriteU16LE(b + 36, m.fade_out_ms);
    b[38] = m.channels;
    b[39] = m.bits_per_sample;
    b[40] = m.loop_enabled;
    b[41] = m.channel_mode;
    b[42] = m.loop_crossfade_ms;
    Wxcf::Writer writer(io);
    return writer.WriteHeader(kFileType, kVersion, kFileBytes) == Wxcf::Result::Ok &&
           writer.WriteChunk(1, kVersion, b, sizeof(b)) == Wxcf::Result::Ok;
}
// Bounded sidecar (4 KiB maximum), foreground only. Decode privately; failure
// leaves the caller's document untouched, including on a truncated extension.
inline bool Read(Wxcf::IoContext io, uint32_t bytes, Document& out) {
    if (!io.read || !io.eof || bytes < kFileBytes || bytes > kMaxFileBytes)
        return false;
    Wxcf::Reader reader(io);
    uint16_t type = 0, version = 0;
    uint32_t declared = 0, remaining = bytes - 12;
    if (reader.ReadHeader(type, version, declared) != Wxcf::Result::Ok || type != kFileType ||
        Wxcf::VersionMajor(version) != 1 || (declared && declared != bytes))
        return false;
    Document candidate;
    bool found = false;
    while (remaining) {
        Wxcf::ChunkHeader chunk;
        if (remaining < 8 || reader.NextChunkHeader(chunk) != Wxcf::Result::Ok)
            return false;
        remaining -= 8;
        if (chunk.payload_len > remaining)
            return false;
        uint32_t skip = chunk.payload_len;
        if (chunk.chunk_id == 1) {
            if (found || (chunk.chunk_version != 0x0100 && chunk.chunk_version != kVersion) ||
                chunk.payload_len < kPayloadBytes)
                return false;
            uint8_t b[kPayloadBytes];
            if (reader.ReadPayload(b, sizeof(b)) != Wxcf::Result::Ok ||
                (chunk.chunk_version == 0x0100 && b[42]) || b[43])
                return false;
            using namespace Wxcf::detail;
            candidate.file_bytes = ReadU32LE(b);
            candidate.data_offset = ReadU32LE(b + 4);
            auto& m = candidate.sample;
            m.sample_rate = ReadU32LE(b + 8);
            m.total_frames = ReadU32LE(b + 12);
            m.start_frame = ReadU32LE(b + 16);
            m.end_frame = ReadU32LE(b + 20);
            m.loop_start = ReadU32LE(b + 24);
            m.loop_end = ReadU32LE(b + 28);
            const uint16_t gain = ReadU16LE(b + 32);
            m.gain_db_x10 = static_cast<int16_t>(gain < 32768 ? gain : int32_t(gain) - 65536);
            m.fade_in_ms = ReadU16LE(b + 34);
            m.fade_out_ms = ReadU16LE(b + 36);
            m.channels = b[38];
            m.bits_per_sample = b[39];
            m.loop_enabled = b[40];
            m.channel_mode = b[41];
            m.loop_crossfade_ms = b[42];
            found = true;
            skip -= kPayloadBytes;
        }
        if (reader.SkipPayload(skip) != Wxcf::Result::Ok)
            return false;
        remaining -= chunk.payload_len;
    }
    if (!found || !io.eof(io.user_data) || !Valid(candidate))
        return false;
    out = candidate;
    return true;
}
}  // namespace WaveX::SampleFile
