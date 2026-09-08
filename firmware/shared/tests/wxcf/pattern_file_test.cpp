#include "wxcf/pattern_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
using namespace WaveX;
using PatternFile::Result;
struct Memory {
    std::vector<uint8_t> bytes;
    size_t pos = 0, transferred = 0, fail_at = SIZE_MAX;
    static bool read(void* context, void* dest, size_t count) {
        auto& m = *static_cast<Memory*>(context);
        if (m.pos + count > m.bytes.size() || m.pos + count > m.fail_at)
            return false;
        std::memcpy(dest, m.bytes.data() + m.pos, count);
        m.pos += count;
        m.transferred += count;
        return true;
    }
    static bool write(void* context, const void* source, size_t count) {
        auto& m = *static_cast<Memory*>(context);
        if (m.bytes.size() + count > m.fail_at)
            return false;
        const auto* b = static_cast<const uint8_t*>(source);
        m.bytes.insert(m.bytes.end(), b, b + count);
        m.transferred += count;
        return true;
    }
    static bool eof(void* context) {
        auto& m = *static_cast<Memory*>(context);
        return m.pos == m.bytes.size();
    }
    Wxcf::IoContext io() { return {this, read, write, eof}; }
};
Memory encoded(const Sequencer::Pattern& pattern) {
    Memory m;
    PatternFile::Encoder encoder(m.io(), pattern, "Night kit");
    Result result = Result::More;
    for (int i = 0; i < 1100 && result == Result::More; ++i)
        result = encoder.Advance();
    EXPECT_EQ(result, Result::Done);
    return m;
}
Result decode(Memory& m, Sequencer::Pattern& pattern, char* name) {
    PatternFile::Decoder decoder(m.io(), pattern, name);
    Result result = Result::More;
    for (int i = 0; i < 3000 && result == Result::More; ++i) {
        m.transferred = 0;
        result = decoder.Advance();
        EXPECT_LE(m.transferred, 128u);
    }
    return result;
}
TEST(PatternFileTest, RoundTripsEveryFieldAndHiddenSteps) {
    Sequencer::Pattern p;
    p.length = 3;
    p.scale = Sequencer::StepScale::EighthTriplet;
    p.swing = 75;
    for (uint8_t t = 0; t < 16; ++t) {
        p.tracks[t].enabled = t % 2;
        for (uint8_t i = 0; i < 64; ++i) {
            auto& s = p.tracks[t].steps[i];
            s.on = i % 2;
            s.note = (t + i) % 128;
            s.velocity = (t * 9 + i) % 128;
            s.probability = i;
            s.micro_offset = static_cast<int16_t>(i % 2 ? -32768 : 32767);
            s.retrig_count = i % 9;
            s.retrig_rate_ticks = static_cast<uint8_t>(i * 4);
            for (uint8_t k = 0; k < 4; ++k)
                s.param_locks[k] = {static_cast<uint8_t>(k + 1),
                                    static_cast<uint16_t>(t * 4000 + i + k)};
        }
    }
    p.tracks[15].steps[63].note = 127;
    auto m = encoded(p);
    EXPECT_EQ(m.bytes.size(), PatternFile::kFileBytes);
    Sequencer::Pattern copy;
    char name[24]{};
    ASSERT_EQ(decode(m, copy, name), Result::Done);
    EXPECT_STREQ(name, "Night kit");
    EXPECT_EQ(copy.length, p.length);
    EXPECT_EQ(copy.scale, p.scale);
    EXPECT_EQ(copy.swing, p.swing);
    for (int t = 0; t < 16; ++t) {
        EXPECT_EQ(copy.tracks[t].enabled, p.tracks[t].enabled);
        for (int s = 0; s < 64; ++s) {
            uint8_t a[20], b[20];
            PatternFile::EncodeStep(p.tracks[t].steps[s], a);
            PatternFile::EncodeStep(copy.tracks[t].steps[s], b);
            EXPECT_EQ(std::memcmp(a, b, 20), 0);
        }
    }
}
TEST(PatternFileTest, RejectsEveryTruncatedPrefixAndIoFailure) {
    auto full = encoded(Sequencer::Pattern{});
    for (size_t n = 0; n < full.bytes.size(); ++n) {
        Memory m;
        m.bytes.assign(full.bytes.begin(), full.bytes.begin() + n);
        Sequencer::Pattern p;
        char name[24]{};
        EXPECT_NE(decode(m, p, name), Result::Done) << n;
    }
    full.fail_at = 100;
    Sequencer::Pattern p;
    char name[24]{};
    EXPECT_NE(decode(full, p, name), Result::Done);
    Memory bad;
    bad.fail_at = 100;
    PatternFile::Encoder w(bad.io(), p, "Name");
    Result result = Result::More;
    for (int i = 0; i < 1100 && result == Result::More; ++i)
        result = w.Advance();
    EXPECT_EQ(result, Result::IoError);
}
TEST(PatternFileTest, RejectsInvalidMetadataStepsAndDuplicateChunks) {
    auto full = encoded(Sequencer::Pattern{});
    const std::pair<size_t, uint8_t> bad[] = {{4, 1},
                                              {7, 2},
                                              {44, 0},
                                              {45, 6},
                                              {46, 49},
                                              {47, 1},
                                              {56, 2},
                                              {57, 2},
                                              {58, 128},
                                              {59, 128},
                                              {60, 101},
                                              {63, 9}};
    for (auto mutation: bad) {
        auto m = full;
        m.bytes[mutation.first] = mutation.second;
        Sequencer::Pattern p;
        char name[24]{};
        EXPECT_NE(decode(m, p, name), Result::Done) << mutation.first;
    }
    auto duplicate = full;
    duplicate.bytes.insert(duplicate.bytes.end(), full.bytes.begin() + 12, full.bytes.begin() + 48);
    Sequencer::Pattern p;
    char name[24]{};
    EXPECT_EQ(decode(duplicate, p, name), Result::Invalid);
    auto partial = full;
    partial.bytes.push_back(0);
    EXPECT_NE(decode(partial, p, name), Result::Done);
}
TEST(PatternFileTest, SkipsUnknownChunksWithBoundedReadsAndRejectsOversize) {
    auto m = encoded(Sequencer::Pattern{});
    Wxcf::Writer extra(m.io());
    std::vector<uint8_t> unknown(4096, 42);
    ASSERT_EQ(extra.WriteChunk(999, 0xff00, unknown.data(), static_cast<uint32_t>(unknown.size())),
              Wxcf::Result::Ok);
    Sequencer::Pattern p;
    char name[24]{};
    EXPECT_EQ(decode(m, p, name), Result::Done);
    auto bad = encoded(p);
    ASSERT_EQ(Wxcf::Writer(bad.io()).BeginChunk(999, 0x100, UINT32_MAX), Wxcf::Result::Ok);
    EXPECT_EQ(decode(bad, p, name), Result::Invalid);
}
TEST(PatternFileTest, NamesAndDuplicateLocksCannotEnterAFile) {
    const char* bad[] = {"", " bad", "bad ", "../bad", "a/b", "123456789012345678901234"};
    Sequencer::Pattern p;
    for (auto name: bad) {
        Memory m;
        PatternFile::Encoder w(m.io(), p, name);
        EXPECT_EQ(w.Advance(), Result::Invalid);
        EXPECT_TRUE(m.bytes.empty());
    }
    p.tracks[0].steps[0].param_locks[0] = {1, 20};
    p.tracks[0].steps[0].param_locks[1] = {1, 30};
    Memory m;
    PatternFile::Encoder w(m.io(), p, "Good");
    EXPECT_EQ(w.Advance(), Result::Invalid);
}
}  // namespace
