#pragma once

// WXCF chunk container (design: docs/features/instrument-model.md §5). One
// versioned TLV file format shared by every WaveX SD artifact: instruments
// (.wxi), tunings (.wxt), scenes/mixer project chunks, and eventually
// patterns/projects (.wxp, docs/features/sequencer.md §3).
//
// Wire format (little-endian throughout):
//   Header := magic "WXCF" (4 B), u16 file_type, u16 file_version, u32 total_len
//   Chunk  := u16 chunk_id, u16 chunk_version, u32 payload_len, payload bytes
// A file is one Header followed by zero or more Chunks back to back, ending
// at EOF (there is no explicit end-of-chunks marker).
//
// HAL-free: all I/O goes through a caller-supplied IoContext (a
// context-pointer + two C function pointers, deliberately not
// std::function - this keeps the header includable from both the C++17
// shared-test build and the Daisy's C++14 firmware build without pulling
// in <functional>'s heap-allocation-capable machinery anywhere near a
// class that HAL wrappers will eventually construct from real SD I/O).
// Round-trips against an in-memory buffer on host (wxcf_test.cpp); a
// FatFs-backed IoContext (wrapping f_read/f_write) is a thin adapter, not
// implemented here.
//
// Atomicity: this class only frames bytes - it has no notion of files,
// paths, or renaming. The "write to <name>.tmp, then f_rename" atomic-save
// discipline (docs/features/offline-sample-editing.md §2) is the
// responsibility of whatever wraps this class with real FatFs calls, not
// this header.
//
// Forward/backward compatibility: a reader that doesn't recognize a
// chunk_id must call SkipPayload() rather than ReadPayload() so newer
// files remain loadable by older firmware (instrument-model.md §5).
// file_version mismatch policy (reject unknown majors) is enforced by the
// caller using VersionMajor()/VersionMinor() - this class only frames the
// bytes, it doesn't know which chunk_ids or versions a given file_type's
// schema considers valid.

#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Wxcf {

// ASCII, not a numeric magic constant - a byte sequence has no endianness
// ambiguity to get wrong, unlike a 4-byte integer would.
static constexpr char kMagic[4] = {'W', 'X', 'C', 'F'};

struct IoContext {
    void* user_data = nullptr;
    // Return false on short read/write or any I/O error. A short
    // read/write is treated identically to a hard error - this format has
    // no resumable-partial-operation concept.
    bool (*read)(void* user_data, void* dest, size_t len) = nullptr;
    bool (*write)(void* user_data, const void* src, size_t len) = nullptr;
};

enum class Result : uint8_t {
    Ok,
    IoError,   // read/write callback returned false (includes EOF)
    BadMagic,  // header's first 4 bytes weren't "WXCF"
};

inline uint8_t VersionMajor(uint16_t version) {
    return static_cast<uint8_t>(version >> 8);
}
inline uint8_t VersionMinor(uint16_t version) {
    return static_cast<uint8_t>(version & 0xFF);
}
inline uint16_t MakeVersion(uint8_t major, uint8_t minor) {
    return static_cast<uint16_t>((static_cast<uint16_t>(major) << 8) | minor);
}

struct ChunkHeader {
    uint16_t chunk_id = 0;
    uint16_t chunk_version = 0;
    uint32_t payload_len = 0;
};

namespace detail {

inline void WriteU16LE(uint8_t* dest, uint16_t v) {
    dest[0] = static_cast<uint8_t>(v & 0xFF);
    dest[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
inline uint16_t ReadU16LE(const uint8_t* src) {
    return static_cast<uint16_t>(static_cast<uint16_t>(src[0]) |
                                 (static_cast<uint16_t>(src[1]) << 8));
}
inline void WriteU32LE(uint8_t* dest, uint32_t v) {
    dest[0] = static_cast<uint8_t>(v & 0xFF);
    dest[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
inline uint32_t ReadU32LE(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace detail

static constexpr size_t kHeaderSize = 12;      // 4 magic + 2 + 2 + 4
static constexpr size_t kChunkHeaderSize = 8;  // 2 + 2 + 4

// Sequential, forward-only writer: WriteHeader() once, then WriteChunk()
// per chunk, in whatever order the caller's schema defines.
class Writer {
   public:
    explicit Writer(IoContext io) : io_(io) {}

    // `total_len` is advisory metadata only (not re-verified by Reader) -
    // pass 0 if unknown at header-write time. WXCF files in this codebase
    // (instruments, tunings, scenes) are small enough that a caller who
    // wants an accurate total either buffers chunk sizes ahead of time or
    // simply doesn't bother; there is no seek-back-and-patch support here.
    Result WriteHeader(uint16_t file_type, uint16_t file_version, uint32_t total_len) {
        uint8_t buf[kHeaderSize];
        buf[0] = static_cast<uint8_t>(kMagic[0]);
        buf[1] = static_cast<uint8_t>(kMagic[1]);
        buf[2] = static_cast<uint8_t>(kMagic[2]);
        buf[3] = static_cast<uint8_t>(kMagic[3]);
        detail::WriteU16LE(buf + 4, file_type);
        detail::WriteU16LE(buf + 6, file_version);
        detail::WriteU32LE(buf + 8, total_len);
        if (!io_.write(io_.user_data, buf, sizeof(buf)))
            return Result::IoError;
        return Result::Ok;
    }

    Result WriteChunk(uint16_t chunk_id,
                      uint16_t chunk_version,
                      const void* payload,
                      uint32_t payload_len) {
        uint8_t buf[kChunkHeaderSize];
        detail::WriteU16LE(buf + 0, chunk_id);
        detail::WriteU16LE(buf + 2, chunk_version);
        detail::WriteU32LE(buf + 4, payload_len);
        if (!io_.write(io_.user_data, buf, sizeof(buf)))
            return Result::IoError;
        if (payload_len > 0 && !io_.write(io_.user_data, payload, payload_len))
            return Result::IoError;
        return Result::Ok;
    }

   private:
    IoContext io_;
};

// Sequential, forward-only reader: ReadHeader() once, then repeatedly
// NextChunkHeader() + (ReadPayload() or SkipPayload()) until
// NextChunkHeader() returns IoError, which at a well-formed EOF boundary
// means "no more chunks" rather than corruption. (This format has no
// explicit chunk count or end marker - see the class comment.)
class Reader {
   public:
    explicit Reader(IoContext io) : io_(io) {}

    Result ReadHeader(uint16_t& file_type, uint16_t& file_version, uint32_t& total_len) {
        uint8_t buf[kHeaderSize];
        if (!io_.read(io_.user_data, buf, sizeof(buf)))
            return Result::IoError;
        if (buf[0] != static_cast<uint8_t>(kMagic[0]) ||
            buf[1] != static_cast<uint8_t>(kMagic[1]) ||
            buf[2] != static_cast<uint8_t>(kMagic[2]) || buf[3] != static_cast<uint8_t>(kMagic[3]))
            return Result::BadMagic;
        file_type = detail::ReadU16LE(buf + 4);
        file_version = detail::ReadU16LE(buf + 6);
        total_len = detail::ReadU32LE(buf + 8);
        return Result::Ok;
    }

    // Reads the next chunk's framing only, not its payload. The caller
    // must dispose of exactly `out.payload_len` bytes (via ReadPayload()
    // or SkipPayload()) before calling this again - the stream position
    // is byte-exact, there is no buffering or lookahead.
    Result NextChunkHeader(ChunkHeader& out) {
        uint8_t buf[kChunkHeaderSize];
        if (!io_.read(io_.user_data, buf, sizeof(buf)))
            return Result::IoError;
        out.chunk_id = detail::ReadU16LE(buf + 0);
        out.chunk_version = detail::ReadU16LE(buf + 2);
        out.payload_len = detail::ReadU32LE(buf + 4);
        return Result::Ok;
    }

    // `dest` must be at least `len` bytes (the chunk's own payload_len,
    // or a caller-known prefix of it if intentionally reading a truncated
    // view - not recommended; prefer SkipPayload for anything not fully
    // understood).
    Result ReadPayload(void* dest, uint32_t len) {
        if (len == 0)
            return Result::Ok;
        if (!io_.read(io_.user_data, dest, len))
            return Result::IoError;
        return Result::Ok;
    }

    // Discards `len` bytes without requiring a caller-provided buffer -
    // this is what an old reader calls on an unrecognized chunk_id, the
    // mechanism forward-compatibility depends on (instrument-model.md §5).
    Result SkipPayload(uint32_t len) {
        uint8_t scratch[64];
        while (len > 0) {
            uint32_t n = len < sizeof(scratch) ? len : static_cast<uint32_t>(sizeof(scratch));
            if (!io_.read(io_.user_data, scratch, n))
                return Result::IoError;
            len -= n;
        }
        return Result::Ok;
    }

   private:
    IoContext io_;
};

}  // namespace Wxcf
}  // namespace WaveX
