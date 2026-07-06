#include "wxcf/wxcf.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using WaveX::Wxcf::ChunkHeader;
using WaveX::Wxcf::IoContext;
using WaveX::Wxcf::MakeVersion;
using WaveX::Wxcf::Reader;
using WaveX::Wxcf::Result;
using WaveX::Wxcf::VersionMajor;
using WaveX::Wxcf::VersionMinor;
using WaveX::Wxcf::Writer;

namespace {

// In-memory backing store for round-trip tests. A short read (asking for
// more bytes than remain) is treated as an I/O error, matching the
// contract wxcf.hpp documents for its IoContext callbacks - this lets
// "read past the last chunk" double as the EOF/no-more-chunks signal
// tests rely on.
struct MemoryIo {
    std::vector<uint8_t> buf;
    size_t read_pos = 0;

    static bool Write(void* self, const void* src, size_t len) {
        auto* m = static_cast<MemoryIo*>(self);
        const uint8_t* p = static_cast<const uint8_t*>(src);
        m->buf.insert(m->buf.end(), p, p + len);
        return true;
    }

    static bool Read(void* self, void* dest, size_t len) {
        auto* m = static_cast<MemoryIo*>(self);
        if (m->read_pos + len > m->buf.size())
            return false;
        std::memcpy(dest, m->buf.data() + m->read_pos, len);
        m->read_pos += len;
        return true;
    }

    IoContext AsWriter() {
        IoContext io;
        io.user_data = this;
        io.write = &Write;
        return io;
    }

    IoContext AsReader() {
        IoContext io;
        io.user_data = this;
        io.read = &Read;
        return io;
    }
};

}  // namespace

TEST(WxcfTest, RoundTripsHeaderOnly) {
    MemoryIo mem;
    Writer w(mem.AsWriter());
    ASSERT_EQ(w.WriteHeader(/*file_type=*/1, MakeVersion(1, 0), /*total_len=*/0), Result::Ok);

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    ASSERT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::Ok);
    EXPECT_EQ(file_type, 1);
    EXPECT_EQ(file_version, MakeVersion(1, 0));
}

TEST(WxcfTest, RoundTripsHeaderAndMultipleChunks) {
    MemoryIo mem;
    Writer w(mem.AsWriter());
    ASSERT_EQ(w.WriteHeader(2, MakeVersion(1, 0), 0), Result::Ok);

    const char zone_payload[] = "zone-payload-bytes";
    const uint8_t mod_payload[] = {1, 2, 3, 4, 5};

    ASSERT_EQ(w.WriteChunk(1, MakeVersion(1, 0), zone_payload, sizeof(zone_payload)), Result::Ok);
    ASSERT_EQ(w.WriteChunk(2, MakeVersion(1, 0), mod_payload, sizeof(mod_payload)), Result::Ok);
    ASSERT_EQ(w.WriteChunk(3, MakeVersion(1, 0), nullptr, 0), Result::Ok);  // empty chunk

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    ASSERT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::Ok);
    EXPECT_EQ(file_type, 2);

    ChunkHeader ch{};
    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.chunk_id, 1);
    EXPECT_EQ(ch.payload_len, sizeof(zone_payload));
    std::vector<char> zone_out(ch.payload_len);
    ASSERT_EQ(r.ReadPayload(zone_out.data(), ch.payload_len), Result::Ok);
    EXPECT_STREQ(zone_out.data(), zone_payload);

    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.chunk_id, 2);
    EXPECT_EQ(ch.payload_len, sizeof(mod_payload));
    std::vector<uint8_t> mod_out(ch.payload_len);
    ASSERT_EQ(r.ReadPayload(mod_out.data(), ch.payload_len), Result::Ok);
    EXPECT_EQ(std::memcmp(mod_out.data(), mod_payload, sizeof(mod_payload)), 0);

    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.chunk_id, 3);
    EXPECT_EQ(ch.payload_len, 0u);
    ASSERT_EQ(r.ReadPayload(nullptr, 0), Result::Ok);  // zero-length payload, no buffer needed

    // No more chunks - reading past the end is the documented EOF signal.
    EXPECT_EQ(r.NextChunkHeader(ch), Result::IoError);
}

TEST(WxcfTest, UnknownChunkIsSkippedAndLaterChunksStillReadCorrectly) {
    MemoryIo mem;
    Writer w(mem.AsWriter());
    ASSERT_EQ(w.WriteHeader(1, MakeVersion(1, 0), 0), Result::Ok);

    const uint8_t future_chunk_payload[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    const char known_payload[] = "known-chunk-data";

    // A hypothetical newer chunk_id (0x9999) that an old reader wouldn't
    // recognize, followed by a chunk the reader's schema does know.
    ASSERT_EQ(
        w.WriteChunk(0x9999, MakeVersion(2, 0), future_chunk_payload, sizeof(future_chunk_payload)),
        Result::Ok);
    ASSERT_EQ(w.WriteChunk(1, MakeVersion(1, 0), known_payload, sizeof(known_payload)), Result::Ok);

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    ASSERT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::Ok);

    ChunkHeader ch{};
    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    ASSERT_EQ(ch.chunk_id, 0x9999);
    // Reader's schema doesn't know chunk 0x9999 - skip without a buffer.
    ASSERT_EQ(r.SkipPayload(ch.payload_len), Result::Ok);

    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.chunk_id, 1);
    std::vector<char> out(ch.payload_len);
    ASSERT_EQ(r.ReadPayload(out.data(), ch.payload_len), Result::Ok);
    EXPECT_STREQ(out.data(), known_payload);
}

TEST(WxcfTest, SkipPayloadHandlesPayloadsLargerThanScratchBuffer) {
    MemoryIo mem;
    Writer w(mem.AsWriter());
    ASSERT_EQ(w.WriteHeader(1, MakeVersion(1, 0), 0), Result::Ok);

    // Larger than the Reader's internal 64-byte skip scratch buffer, to
    // exercise the multi-iteration path in SkipPayload().
    std::vector<uint8_t> big_payload(500);
    for (size_t i = 0; i < big_payload.size(); ++i)
        big_payload[i] = static_cast<uint8_t>(i);
    ASSERT_EQ(
        w.WriteChunk(
            1, MakeVersion(1, 0), big_payload.data(), static_cast<uint32_t>(big_payload.size())),
        Result::Ok);
    ASSERT_EQ(w.WriteChunk(2, MakeVersion(1, 0), "after-skip", 11), Result::Ok);

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    ASSERT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::Ok);

    ChunkHeader ch{};
    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.payload_len, 500u);
    ASSERT_EQ(r.SkipPayload(ch.payload_len), Result::Ok);

    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.chunk_id, 2);
    char out[11];
    ASSERT_EQ(r.ReadPayload(out, 11), Result::Ok);
    EXPECT_STREQ(out, "after-skip");
}

TEST(WxcfTest, BadMagicIsRejected) {
    MemoryIo mem;
    // Hand-craft a header with a wrong magic.
    const uint8_t bad_header[] = {
        'X',
        'X',
        'X',
        'X',  // wrong magic
        1,
        0,  // file_type
        0,
        1,  // file_version
        0,
        0,
        0,
        0,  // total_len
    };
    mem.buf.assign(bad_header, bad_header + sizeof(bad_header));

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    EXPECT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::BadMagic);
}

TEST(WxcfTest, TruncatedHeaderIsIoError) {
    MemoryIo mem;
    mem.buf = {'W', 'X', 'C', 'F', 1, 0};  // only 6 of 12 header bytes

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    EXPECT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::IoError);
}

TEST(WxcfTest, TruncatedChunkPayloadIsIoError) {
    MemoryIo mem;
    Writer w(mem.AsWriter());
    ASSERT_EQ(w.WriteHeader(1, MakeVersion(1, 0), 0), Result::Ok);
    ASSERT_EQ(w.WriteChunk(1, MakeVersion(1, 0), "0123456789", 10), Result::Ok);

    // Truncate the file mid-payload.
    mem.buf.resize(mem.buf.size() - 4);

    Reader r(mem.AsReader());
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    ASSERT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::Ok);

    ChunkHeader ch{};
    ASSERT_EQ(r.NextChunkHeader(ch), Result::Ok);
    EXPECT_EQ(ch.payload_len, 10u);
    char out[10];
    EXPECT_EQ(r.ReadPayload(out, 10), Result::IoError);
}

TEST(WxcfTest, VersionMajorMinorRoundTrip) {
    uint16_t v = MakeVersion(3, 7);
    EXPECT_EQ(VersionMajor(v), 3);
    EXPECT_EQ(VersionMinor(v), 7);

    // Boundary values.
    uint16_t max_v = MakeVersion(255, 255);
    EXPECT_EQ(VersionMajor(max_v), 255);
    EXPECT_EQ(VersionMinor(max_v), 255);
}

TEST(WxcfTest, LittleEndianByteOrderOnWire) {
    MemoryIo mem;
    Writer w(mem.AsWriter());
    // file_type=0x0102, file_version=0x0304, total_len=0x05060708
    ASSERT_EQ(w.WriteHeader(0x0102, 0x0304, 0x05060708), Result::Ok);

    ASSERT_EQ(mem.buf.size(), 12u);
    // Magic.
    EXPECT_EQ(mem.buf[0], 'W');
    EXPECT_EQ(mem.buf[1], 'X');
    EXPECT_EQ(mem.buf[2], 'C');
    EXPECT_EQ(mem.buf[3], 'F');
    // file_type, little-endian.
    EXPECT_EQ(mem.buf[4], 0x02);
    EXPECT_EQ(mem.buf[5], 0x01);
    // file_version, little-endian.
    EXPECT_EQ(mem.buf[6], 0x04);
    EXPECT_EQ(mem.buf[7], 0x03);
    // total_len, little-endian.
    EXPECT_EQ(mem.buf[8], 0x08);
    EXPECT_EQ(mem.buf[9], 0x07);
    EXPECT_EQ(mem.buf[10], 0x06);
    EXPECT_EQ(mem.buf[11], 0x05);
}

TEST(WxcfTest, WriteFailurePropagatesAsIoError) {
    IoContext io;
    io.user_data = nullptr;
    io.write = [](void*, const void*, size_t) -> bool { return false; };
    Writer w(io);
    EXPECT_EQ(w.WriteHeader(1, MakeVersion(1, 0), 0), Result::IoError);
}

TEST(WxcfTest, ReadFailurePropagatesAsIoError) {
    IoContext io;
    io.user_data = nullptr;
    io.read = [](void*, void*, size_t) -> bool { return false; };
    Reader r(io);
    uint16_t file_type = 0, file_version = 0;
    uint32_t total_len = 0;
    EXPECT_EQ(r.ReadHeader(file_type, file_version, total_len), Result::IoError);
}
