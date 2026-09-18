#include "wxcf/bank_file.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>
namespace {
using namespace WaveX;
using R = BankFile::Result;
struct Memory {
    std::vector<uint8_t> data;
    size_t pos = 0, transferred = 0, fail_at = SIZE_MAX;
    static bool Read(void* ctx, void* dest, size_t n) {
        auto& m = *static_cast<Memory*>(ctx);
        if (n > m.data.size() - m.pos || m.pos + n > m.fail_at)
            return false;
        std::memcpy(dest, m.data.data() + m.pos, n);
        m.pos += n;
        m.transferred += n;
        return true;
    }
    static bool Write(void* ctx, const void* src, size_t n) {
        auto& m = *static_cast<Memory*>(ctx);
        if (m.data.size() + n > m.fail_at)
            return false;
        const auto* b = static_cast<const uint8_t*>(src);
        m.data.insert(m.data.end(), b, b + n);
        m.transferred += n;
        return true;
    }
    static bool Eof(void* ctx) {
        auto& m = *static_cast<Memory*>(ctx);
        return m.pos == m.data.size();
    }
    Wxcf::IoContext Io() { return {this, Read, Write, Eof}; }
};
R Scan(Memory& m, BankFile::Index& idx) {
    m.pos = 0;
    BankFile::IndexDecoder decoder(m.Io(), idx);
    auto r = R::More;
    for (unsigned n = 0; n < 40000 && r == R::More; ++n) {
        m.transferred = 0;
        r = decoder.Advance();
        EXPECT_LE(m.transferred, 128u);
    }
    return r;
}
std::unique_ptr<Wxi::InstrumentFile> Document() {
    auto doc = std::make_unique<Wxi::InstrumentFile>();
    std::strcpy(doc->name, "Layered keys");
    doc->tags = 3;
    doc->osc[0].zone_count = 1;
    std::strcpy(doc->osc[0].zones[0].path, "/keys.wav");
    doc->osc[1].type = Wxi::OscType::Sample;
    doc->osc[1].zone_count = 1;
    std::strcpy(doc->osc[1].zones[0].path, "/click.wav");
    doc->env[2].attack_s = .4f;
    doc->lfo[1].rate_hz = 3.0f;
    return doc;
}
Memory Sparse() {
    Memory m;
    BankFile::Encoder w(m.Io());
    const auto doc = Document();
    EXPECT_EQ(w.Begin("Live bank"), R::More);
    EXPECT_EQ(w.Append(0, *doc), R::More);
    EXPECT_EQ(w.Append(127, *doc), R::More);
    EXPECT_EQ(w.Finish(), R::Done);
    return m;
}
TEST(BankFile, SparseSlotsReuseWxiAndKeepBothOscillatorsAndModulators) {
    auto m = Sparse();
    BankFile::Index idx;
    ASSERT_EQ(Scan(m, idx), R::Done);
    EXPECT_STREQ(idx.name, "Live bank");
    EXPECT_TRUE(idx.slots[0].used());
    EXPECT_TRUE(idx.slots[127].used());
    EXPECT_FALSE(idx.slots[1].used());
    auto recalled = std::make_unique<Wxi::InstrumentFile>();
    for (unsigned slot: {0u, 127u}) {
        m.pos = idx.slots[slot].offset;
        ASSERT_EQ(BankFile::ReadSlot(m.Io(), idx.slots[slot], *recalled), R::Done);
        EXPECT_EQ(m.pos, idx.slots[slot].offset + idx.slots[slot].bytes);
        EXPECT_STREQ(recalled->osc[1].zones[0].path, "/click.wav");
        EXPECT_FLOAT_EQ(recalled->env[2].attack_s, .4f);
        EXPECT_FLOAT_EQ(recalled->lfo[1].rate_hz, 3);
    }
}
TEST(BankFile, ReadSlotRejectsMismatchedMetadataAndDoesNotReadPastItsBoundary) {
    auto m = Sparse();
    BankFile::Index idx;
    ASSERT_EQ(Scan(m, idx), R::Done);
    auto out = std::make_unique<Wxi::InstrumentFile>();
    auto wrong = idx.slots[0];
    wrong.name[0] = 'X';
    m.pos = wrong.offset;
    EXPECT_EQ(BankFile::ReadSlot(m.Io(), wrong, *out), R::Invalid);
    m.pos = idx.slots[0].offset;
    m.fail_at = m.pos + 30;
    EXPECT_EQ(BankFile::ReadSlot(m.Io(), idx.slots[0], *out), R::IoError);
    m.fail_at = SIZE_MAX;
    auto short_slot = idx.slots[0];
    --short_slot.bytes;
    m.pos = short_slot.offset;
    EXPECT_NE(BankFile::ReadSlot(m.Io(), short_slot, *out), R::Done);
    EXPECT_LE(m.pos, short_slot.offset + short_slot.bytes);
}
TEST(BankFile, IndexRejectsTruncationAndDuplicateSlotsButSkipsFutureChunks) {
    const auto original = Sparse();
    BankFile::Index idx;
    auto m = original;
    m.data.pop_back();
    EXPECT_NE(Scan(m, idx), R::Done);
    m = original;
    m.data.insert(m.data.end(), original.data.begin() + 44, original.data.end());
    EXPECT_EQ(Scan(m, idx), R::Invalid);
    m = original;
    Wxcf::Writer extra(m.Io());
    uint8_t future[400]{};
    ASSERT_EQ(extra.WriteChunk(0x7000, 0x0200, future, sizeof(future)), Wxcf::Result::Ok);
    EXPECT_EQ(Scan(m, idx), R::Done);
    m = original;
    m.data[7] = 2;
    EXPECT_EQ(Scan(m, idx), R::Invalid);
    m = original;
    Wxcf::detail::WriteU32LE(m.data.data() + 8, static_cast<uint32_t>(m.data.size() + 1));
    EXPECT_EQ(Scan(m, idx), R::Invalid);
}
TEST(BankFile, EncoderErrorsAreTerminalAndDuplicateSlotsAreRejected) {
    auto doc = Document();
    Memory m;
    BankFile::Encoder writer(m.Io());
    ASSERT_EQ(writer.Begin("Bank"), R::More);
    ASSERT_EQ(writer.Append(3, *doc), R::More);
    EXPECT_EQ(writer.Append(3, *doc), R::Invalid);
    const auto n = m.data.size();
    EXPECT_EQ(writer.Begin("Retry"), R::Invalid);
    EXPECT_EQ(writer.Finish(), R::Invalid);
    EXPECT_EQ(m.data.size(), n);
    Memory failing;
    failing.fail_at = 100;
    BankFile::Encoder failed(failing.Io());
    ASSERT_EQ(failed.Begin("Bank"), R::More);
    EXPECT_EQ(failed.Append(0, *doc), R::IoError);
    EXPECT_EQ(failed.Finish(), R::IoError);
}
TEST(BankFile, FullCapacityRemainsSmallAndIndexReadsAreBounded) {
    auto doc = Document();
    Memory full;
    BankFile::Encoder writer(full.Io());
    ASSERT_EQ(writer.Begin("Full bank"), R::More);
    for (auto& osc: doc->osc) {
        osc.zone_count = 32;
        for (unsigned i = 0; i < 32; ++i) {
            osc.zones[i].index = i;
            std::strcpy(osc.zones[i].path, "/keys.wav");
        }
    }
    for (unsigned i = 0; i < 128; ++i)
        ASSERT_EQ(writer.Append(i, *doc), R::More);
    ASSERT_EQ(writer.Finish(), R::Done);
    EXPECT_LT(full.data.size(), BankFile::kMaxFileBytes);
    BankFile::Index idx;
    ASSERT_EQ(Scan(full, idx), R::Done);
    EXPECT_LT(sizeof(idx), 6u * 1024u);
    for (const auto& slot: idx.slots)
        EXPECT_TRUE(slot.used());
}
TEST(BankFile, EmptyBankIsValidAndDoesNotRetainAnEarlierIndex) {
    auto m = Sparse();
    BankFile::Index idx;
    ASSERT_EQ(Scan(m, idx), R::Done);
    Memory empty;
    BankFile::Encoder writer(empty.Io());
    ASSERT_EQ(writer.Begin("Empty"), R::More);
    ASSERT_EQ(writer.Finish(), R::Done);
    ASSERT_EQ(Scan(empty, idx), R::Done);
    for (const auto& slot: idx.slots)
        EXPECT_FALSE(slot.used());
}
TEST(BankFile, SerializedCopyIsBoundedAndPreservesEmbeddedBytes) {
    auto source = Sparse();
    BankFile::Index index;
    ASSERT_EQ(Scan(source, index), R::Done);
    Memory destination;
    BankFile::Encoder writer(destination.Io());
    ASSERT_EQ(writer.Begin("Copied"), R::More);
    source.pos = index.slots[127].offset;
    ASSERT_EQ(writer.BeginCopy(7, index.slots[127]), R::More);
    while (writer.Copying()) {
        source.transferred = destination.transferred = 0;
        ASSERT_EQ(writer.CopyNext(source.Io()), R::More);
        EXPECT_LE(source.transferred, 128u);
        EXPECT_EQ(source.transferred, destination.transferred);
    }
    ASSERT_EQ(writer.Finish(), R::Done);
    BankFile::Index copied;
    ASSERT_EQ(Scan(destination, copied), R::Done);
    ASSERT_TRUE(copied.slots[7].used());
    EXPECT_FALSE(copied.slots[127].used());
    EXPECT_EQ(copied.slots[7].bytes, index.slots[127].bytes);
    EXPECT_EQ(std::memcmp(source.data.data() + index.slots[127].offset,
                          destination.data.data() + copied.slots[7].offset,
                          copied.slots[7].bytes),
              0);
}
TEST(BankFile, UnfinishedOrFailedCopiesCannotProduceCompletedBanks) {
    auto source = Sparse();
    BankFile::Index index;
    ASSERT_EQ(Scan(source, index), R::Done);
    Memory destination;
    BankFile::Encoder writer(destination.Io());
    ASSERT_EQ(writer.Begin("Copy"), R::More);
    ASSERT_EQ(writer.BeginCopy(0, index.slots[0]), R::More);
    EXPECT_EQ(writer.Finish(), R::Invalid);
    Memory failed;
    BankFile::Encoder broken(failed.Io());
    ASSERT_EQ(broken.Begin("Copy"), R::More);
    ASSERT_EQ(broken.BeginCopy(0, index.slots[0]), R::More);
    source.pos = index.slots[0].offset;
    source.fail_at = source.pos;
    EXPECT_EQ(broken.CopyNext(source.Io()), R::IoError);
    const auto size = failed.data.size();
    EXPECT_EQ(broken.Finish(), R::IoError);
    EXPECT_EQ(broken.CopyNext(source.Io()), R::IoError);
    EXPECT_EQ(failed.data.size(), size);
}
}  // namespace
