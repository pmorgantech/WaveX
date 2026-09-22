#pragma once
#include "wxi.hpp"

namespace WaveX::Wxi {
// Bounded metadata reader: the caller performs one offset/size read per step.
// Skips sample maps without loading them; tags do not imply the sound is loadable.
class TagScan {
   public:
    void Begin(uint32_t bytes) {
        *this = {};
        bytes_ = bytes;
        done_ = bytes < Wxcf::kHeaderSize;
    }
    uint32_t Offset() const { return offset_; }
    uint32_t Size() const {
        return phase_ == Header  ? Wxcf::kHeaderSize
               : phase_ == Chunk ? Wxcf::kChunkHeaderSize
                                 : kHeadWireSize;
    }
    bool Done() const { return done_; }
    bool Valid() const { return done_ && valid_; }
    uint8_t Tags() const { return tags_; }
    void Fail() {
        done_ = true;
        valid_ = false;
    }
    void Accept(const uint8_t* data, size_t size) {
        if (done_)
            return;
        if (!data || size != Size() || size > bytes_ - offset_) {
            Fail();
            return;
        }
        using namespace Wxcf::detail;
        if (phase_ == Header) {
            const uint32_t declared = ReadU32LE(data + 8);
            if (std::memcmp(data, Wxcf::kMagic, 4) || ReadU16LE(data + 4) != kFileType ||
                Wxcf::VersionMajor(ReadU16LE(data + 6)) != Wxcf::VersionMajor(kFileVersion) ||
                (declared && declared != bytes_)) {
                Fail();
                return;
            }
            Advance(Wxcf::kHeaderSize);
        } else if (phase_ == Chunk) {
            const uint32_t payload = ReadU32LE(data + 4);
            const uint32_t start = offset_ + Wxcf::kChunkHeaderSize;
            if (payload > bytes_ - start) {
                Fail();
                return;
            }
            next_ = start + payload;
            if (ReadU16LE(data) == kChunkHead) {
                if (payload < kHeadWireSize) {
                    Fail();
                    return;
                }
                phase_ = Head;
                offset_ = start;
            } else
                Advance(next_);
        } else {
            tags_ = data[24];
            saw_head_ = true;
            Advance(next_);
        }
    }

   private:
    void Advance(uint32_t offset) {
        offset_ = offset;
        phase_ = Chunk;
        if (offset_ == bytes_) {
            done_ = true;
            valid_ = saw_head_;
        } else if (bytes_ - offset_ < Wxcf::kChunkHeaderSize)
            Fail();
    }
    enum Phase { Header, Chunk, Head } phase_ = Header;
    uint32_t bytes_ = 0, offset_ = 0, next_ = 0;
    uint8_t tags_ = 0;
    bool done_ = false, valid_ = false, saw_head_ = false;
};
}  // namespace WaveX::Wxi
