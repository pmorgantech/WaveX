#pragma once

// Shared RIFF/WAV header parser (review M12). The tree carried three
// hand-rolled copies of the same chunk walk (audio_engine.cpp's OpenWav and
// OnSampleLoad, daisy_filesystem.cpp's ParseWavMetadata) - and the two
// audio_engine copies skipped odd-sized chunks WITHOUT the RIFF pad byte,
// so any WAV with an odd-length LIST/INFO chunk before `data` mis-parsed.
// One implementation, host-tested (tests/wav/wav_header_parser_test.cpp).
//
// HAL-free: templated over a Reader providing
//   size_t Read(void* dst, size_t n);   // returns bytes read
//   bool   Seek(uint32_t abs_offset);   // absolute; false on failure
//   uint32_t Tell() const;
// A MemReader for in-memory probes (and host tests) is provided below; the
// FatFS adapter lives in firmware/daisy/src/storage/fatfs_wav_reader.hpp.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaveX {
namespace Wav {

struct WavInfo {
    uint16_t audio_format = 0;  // 1 = integer PCM
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset = 0;  // absolute byte offset of the data payload
    uint32_t data_size = 0;    // bytes in the data chunk
};

/**
 * @brief Playing time of a parsed WAV, in milliseconds. 0 if undeterminable.
 *
 * Lives here, next to the struct it reads, because the obvious inline version
 * is wrong: `(frames * 1000u) / sample_rate` in 32-bit overflows once frames
 * exceeds 4,294,967 - only 97 seconds at 44.1 kHz - and the wrapped result
 * bears no visible relationship to the truth, so it reads as a plausible
 * duration rather than as garbage. That shipped, and three-minute songs
 * listed as 38 seconds.
 */
inline uint32_t DurationMs(const WavInfo& info) {
    if (info.sample_rate == 0 || info.num_channels == 0 || info.bits_per_sample < 8) {
        return 0;
    }
    const uint32_t bytes_per_frame = (info.bits_per_sample / 8u) * info.num_channels;
    if (bytes_per_frame == 0) {
        return 0;
    }
    const uint64_t frames = info.data_size / bytes_per_frame;
    return static_cast<uint32_t>((frames * 1000ull) / info.sample_rate);
}

enum class ParseResult : uint8_t {
    Ok,
    NotRiffWave,  // missing RIFF/WAVE magic
    IoError,      // short read or seek failure mid-walk
    NoFmtChunk,   // stream ended without a fmt chunk
    NoDataChunk,  // stream ended without a data chunk
    BadFmtChunk,  // fmt chunk smaller than the 16 mandatory bytes
};

namespace detail {
inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace detail

// Walks the chunk list from offset 0 until both `fmt ` and `data` are
// found. RIFF chunks are word-aligned: an odd-sized chunk is followed by
// one pad byte, which is skipped (this is the M12 fix). On failure, `out`
// still holds whatever was parsed before the failure - e.g. a probe that
// ends before the data chunk still yields the fmt fields (callers that
// only want sample-rate/channels can use them; check the fields, not just
// the result).
template <typename Reader>
ParseResult ParseWavHeader(Reader& r, WavInfo& out) {
    out = WavInfo{};

    uint8_t hdr[12];
    if (!r.Seek(0) || r.Read(hdr, sizeof(hdr)) != sizeof(hdr)) {
        return ParseResult::IoError;
    }
    if (std::memcmp(hdr + 0, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        return ParseResult::NotRiffWave;
    }

    bool fmt_found = false;
    bool data_found = false;

    // Bounded walk: a corrupt size field must not loop forever. 64 chunks
    // is far beyond anything a sane WAV carries before fmt+data.
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t chunk_hdr[8];
        const size_t n = r.Read(chunk_hdr, sizeof(chunk_hdr));
        if (n != sizeof(chunk_hdr)) {
            break;  // end of stream/probe; report what's missing below
        }
        const uint32_t csz = detail::le32(chunk_hdr + 4);
        const uint32_t pad = csz & 1u;  // RIFF word alignment (M12)

        if (std::memcmp(chunk_hdr, "fmt ", 4) == 0) {
            if (csz < 16) {
                return ParseResult::BadFmtChunk;
            }
            uint8_t fmt[16];
            if (r.Read(fmt, sizeof(fmt)) != sizeof(fmt)) {
                return ParseResult::IoError;
            }
            out.audio_format = detail::le16(fmt + 0);
            out.num_channels = detail::le16(fmt + 2);
            out.sample_rate = detail::le32(fmt + 4);
            out.bits_per_sample = detail::le16(fmt + 14);
            fmt_found = true;
            const uint32_t skip = (csz - 16) + pad;
            if (skip != 0 && !r.Seek(r.Tell() + skip)) {
                return ParseResult::IoError;
            }
        } else if (std::memcmp(chunk_hdr, "data", 4) == 0) {
            out.data_offset = r.Tell();
            out.data_size = csz;
            data_found = true;
            if (fmt_found) {
                return ParseResult::Ok;
            }
            // fmt after data is non-canonical but tolerated: skip the
            // payload and keep walking.
            if (!r.Seek(r.Tell() + csz + pad)) {
                return ParseResult::IoError;
            }
        } else {
            if (!r.Seek(r.Tell() + csz + pad)) {
                return ParseResult::IoError;
            }
        }

        if (fmt_found && data_found) {
            return ParseResult::Ok;
        }
    }

    if (!fmt_found) {
        return ParseResult::NoFmtChunk;
    }
    return data_found ? ParseResult::Ok : ParseResult::NoDataChunk;
}

// Reader over an in-memory buffer (header probes, host tests).
class MemReader {
   public:
    MemReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    size_t Read(void* dst, size_t n) {
        const size_t avail = len_ - pos_;
        if (n > avail) {
            n = avail;
        }
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return n;
    }
    bool Seek(uint32_t abs_offset) {
        if (abs_offset > len_) {
            return false;
        }
        pos_ = abs_offset;
        return true;
    }
    uint32_t Tell() const { return static_cast<uint32_t>(pos_); }

   private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
};

}  // namespace Wav
}  // namespace WaveX
