#pragma once

// Foreground Bank container. Only the index stays resident. Slot documents use
// the existing WXI reader/writer; samples remain path references.
// A caller publishes an index only after IndexDecoder::Advance returns Done.
#include "wxcf/pattern_file.hpp"
#include "wxi/wxi.hpp"
#include <cstring>

namespace WaveX {
namespace BankFile {
using PatternFile::Result;
constexpr uint16_t kFileType = 7, kVersion = 0x0100;
constexpr uint16_t kSlots = 128;
constexpr uint32_t kSlotPrefixBytes = 28;
constexpr uint32_t kMaxDocumentBytes = 64 * 1024;
constexpr uint32_t kMaxFileBytes = 4 * 1024 * 1024;
struct Slot {
    char name[24]{};
    uint8_t tags = 0;
    uint32_t offset = 0, bytes = 0;  // Embedded WXI start and exact length.
    bool used() const { return bytes != 0; }
};
struct Index {
    char name[24]{};
    Slot slots[kSlots]{};
};
static_assert(sizeof(Index) < 6 * 1024, "Bank residency is an index, not 128 Instruments");

inline bool ValidName(const char* name) {
    return PatternFile::ValidName(name);
}

// Sequential new-file writer. One whole Instrument per Append: caller must
// schedule this foreground work and own its WXI scratch. Never callback-safe.
// Copies of unchanged serialized slots belong to the SD transaction adapter.
class Encoder {
   public:
    Encoder(Wxcf::IoContext io) : io_(io), writer_(io) {}
    Result Begin(const char* name) {
        if (result_ != Result::More)
            return result_;
        if (started_ || !ValidName(name))
            return Fail(Result::Invalid);
        char saved[24]{};
        std::strncpy(saved, name, sizeof(saved) - 1);
        if (writer_.WriteHeader(kFileType, kVersion, 0) != Wxcf::Result::Ok ||
            writer_.WriteChunk(1, kVersion, saved, sizeof(saved)) != Wxcf::Result::Ok)
            return Fail(Result::IoError);
        started_ = true;
        return result_;
    }
    Result Append(uint8_t slot, const Wxi::InstrumentFile& doc) {
        if (result_ != Result::More)
            return result_;
        if (!started_ || slot >= kSlots || seen_[slot] || !ValidName(doc.name))
            return Fail(Result::Invalid);
        const uint32_t bytes = Wxi::detail::TotalFileSize(doc);
        if (bytes > kMaxDocumentBytes || written_ > kMaxFileBytes - 8 - kSlotPrefixBytes - bytes)
            return Fail(Result::Invalid);
        uint8_t prefix[kSlotPrefixBytes]{};
        std::memcpy(prefix, doc.name, sizeof(doc.name));
        prefix[24] = doc.tags;
        if (writer_.BeginChunk(static_cast<uint16_t>(0x100 + slot),
                               kVersion,
                               kSlotPrefixBytes + bytes) != Wxcf::Result::Ok ||
            writer_.WriteData(prefix, sizeof(prefix)) != Wxcf::Result::Ok ||
            Wxi::Write(io_, doc) != Wxi::Result::Ok)
            return Fail(Result::IoError);
        seen_[slot] = true;
        written_ += 8 + kSlotPrefixBytes + bytes;
        return result_;
    }
    Result Finish() {
        if (result_ == Result::More)
            result_ = started_ ? Result::Done : Result::Invalid;
        return result_;
    }

   private:
    Result Fail(Result result) { return result_ = result; }
    Wxcf::IoContext io_;
    Wxcf::Writer writer_;
    bool started_ = false, seen_[kSlots]{};
    uint32_t written_ = 12 + 8 + 24;
    Result result_ = Result::More;
};

// Scans framing and names without retaining or decoding every Instrument.
// Slot contents are validated by ReadSlot when recalled. This permits a
// bounded index scan regardless of the number of zones in an Instrument.
class IndexDecoder {
   public:
    IndexDecoder(Wxcf::IoContext io, Index& index) : reader_(io), out_(index) { out_ = Index{}; }
    Result Advance() {
        if (result_ != Result::More)
            return result_;
        if (!started_) {
            uint16_t type = 0, version = 0;
            if (reader_.ReadHeader(type, version, declared_) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            if (type != kFileType || Wxcf::VersionMajor(version) != 1 ||
                declared_ > kMaxFileBytes || (declared_ && declared_ < 12))
                return Fail(Result::Invalid);
            started_ = true;
            return result_;
        }
        if (skip_) {
            const uint32_t n = skip_ < 128 ? skip_ : 128;
            if (reader_.SkipPayload(n) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            skip_ -= n;
            return result_;
        }
        Wxcf::ChunkHeader chunk;
        const auto read = reader_.NextChunkHeader(chunk);
        if (read == Wxcf::Result::EndOfFile)
            return result_ = head_ && (!declared_ || declared_ == consumed_) ? Result::Done
                                                                             : Result::Invalid;
        if (read != Wxcf::Result::Ok)
            return Fail(Result::IoError);
        if (chunk.payload_len > kMaxFileBytes - 8 ||
            consumed_ > kMaxFileBytes - 8 - chunk.payload_len)
            return Fail(Result::Invalid);
        const uint32_t payload_start = consumed_ + 8;
        consumed_ += 8 + chunk.payload_len;
        if (declared_ && consumed_ > declared_)
            return Fail(Result::Invalid);
        if (chunk.chunk_id == 1) {
            if (head_ || chunk.payload_len != sizeof(out_.name) ||
                Wxcf::VersionMajor(chunk.chunk_version) != 1)
                return Fail(Result::Invalid);
            if (reader_.ReadPayload(out_.name, sizeof(out_.name)) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            if (!ValidName(out_.name))
                return Fail(Result::Invalid);
            head_ = true;
        } else if (chunk.chunk_id >= 0x100 && chunk.chunk_id < 0x100 + kSlots) {
            auto& slot = out_.slots[chunk.chunk_id - 0x100];
            if (slot.used() || Wxcf::VersionMajor(chunk.chunk_version) != 1 ||
                chunk.payload_len < kSlotPrefixBytes + 12 ||
                chunk.payload_len > kSlotPrefixBytes + kMaxDocumentBytes)
                return Fail(Result::Invalid);
            uint8_t prefix[kSlotPrefixBytes], header[12];
            if (reader_.ReadPayload(prefix, sizeof(prefix)) != Wxcf::Result::Ok ||
                reader_.ReadPayload(header, sizeof(header)) != Wxcf::Result::Ok)
                return Fail(Result::IoError);
            std::memcpy(slot.name, prefix, sizeof(slot.name));
            slot.tags = prefix[24];
            slot.offset = payload_start + kSlotPrefixBytes;
            slot.bytes = chunk.payload_len - kSlotPrefixBytes;
            const uint32_t length = Wxcf::detail::ReadU32LE(header + 8);
            if (!ValidName(slot.name) || prefix[25] || prefix[26] || prefix[27] ||
                std::memcmp(header, Wxcf::kMagic, 4) ||
                Wxcf::detail::ReadU16LE(header + 4) != Wxi::kFileType ||
                Wxcf::VersionMajor(Wxcf::detail::ReadU16LE(header + 6)) != 1 ||
                (length && length != slot.bytes))
                return Fail(Result::Invalid);
            skip_ = slot.bytes - 12;
        } else
            skip_ = chunk.payload_len;
        return result_;
    }

   private:
    Result Fail(Result result) { return result_ = result; }
    Wxcf::Reader reader_;
    Index& out_;
    uint32_t consumed_ = 12, declared_ = 0, skip_ = 0;
    bool started_ = false, head_ = false;
    Result result_ = Result::More;
};

// Caller positions its read-only stream at slot.offset. A private bounded
// view makes WXI EOF local to the slot, never the whole Bank. Failed reads
// cannot consume the next slot. Destination must be private until Done.
inline Result ReadSlot(Wxcf::IoContext input, const Slot& slot, Wxi::InstrumentFile& out) {
    if (!slot.used() || slot.bytes > kMaxDocumentBytes || !input.read)
        return Result::Invalid;
    struct View {
        Wxcf::IoContext source;
        uint32_t remaining;
        static bool Read(void* ctx, void* dest, size_t bytes) {
            auto& v = *static_cast<View*>(ctx);
            if (bytes > v.remaining || !v.source.read(v.source.user_data, dest, bytes))
                return false;
            v.remaining -= static_cast<uint32_t>(bytes);
            return true;
        }
        static bool Eof(void* ctx) { return static_cast<View*>(ctx)->remaining == 0; }
    } view{input, slot.bytes};
    const auto result = Wxi::Read({&view, View::Read, nullptr, View::Eof}, out);
    if (result != Wxi::Result::Ok)
        return result == Wxi::Result::IoError ? Result::IoError : Result::Invalid;
    if (view.remaining || std::memcmp(out.name, slot.name, sizeof(slot.name)) ||
        out.tags != slot.tags)
        return Result::Invalid;
    return Result::Done;
}
}  // namespace BankFile
}  // namespace WaveX
