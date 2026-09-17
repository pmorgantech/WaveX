#include "wxcf/project_file.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace {
using namespace WaveX;
using ProjectFile::Result;
struct Memory {
    std::vector<uint8_t> bytes;
    size_t position = 0, transferred = 0, fail_at = SIZE_MAX;
    static bool Read(void* ctx, void* dest, size_t n) {
        auto& m = *static_cast<Memory*>(ctx);
        if (m.position + n > m.bytes.size() || m.position + n > m.fail_at)
            return false;
        std::memcpy(dest, m.bytes.data() + m.position, n);
        m.position += n;
        m.transferred += n;
        return true;
    }
    static bool Write(void* ctx, const void* data, size_t n) {
        auto& m = *static_cast<Memory*>(ctx);
        if (m.bytes.size() + n > m.fail_at)
            return false;
        const auto* b = static_cast<const uint8_t*>(data);
        m.bytes.insert(m.bytes.end(), b, b + n);
        m.transferred += n;
        return true;
    }
    static bool Eof(void* ctx) {
        auto& m = *static_cast<Memory*>(ctx);
        return m.position == m.bytes.size();
    }
    Wxcf::IoContext Io() { return {this, Read, Write, Eof}; }
};
std::unique_ptr<Sequencer::Project> Example() {
    auto p = std::make_unique<Sequencer::Project>();
    std::strcpy(p->name, "Night set");
    std::strcpy(p->bank_path, "/wavex/banks/Night.wxb");
    p->active_pattern = 127;
    p->selected_song = 15;
    p->patterns[127].used = true;
    std::strcpy(p->patterns[127].name, "Hidden");
    p->patterns[127].pattern.length = 1;
    p->patterns[127].pattern.tracks[15].steps[63].param_locks[3] = {8, 65535};
    p->patterns[127].pattern.tracks[15].steps[63].note = 127;
    auto& song = p->songs[15];
    song.used = true;
    std::strcpy(song.name, "Set finale");
    song.tempo_bpm_x100 = 14325;
    song.length = Sequencer::kMaxSongEntries;
    for (auto& e: song.entries)
        e = {127, 255};
    auto& track = p->tracks[15];
    std::strcpy(track.instrument_path, "/wavex/instruments/Keys.wxi");
    track.midi_in = 16;
    track.poly_limit = 8;
    track.priority = 255;
    track.program_change = false;
    track.mix.gain = .25f;
    track.mix.pan_offset = -1;
    track.mix.mute = true;
    p->master_gain = .75f;
    p->quantize = true;
    return p;
}
template <class Codec>
Result AdvanceAll(Codec& codec, Memory& memory) {
    Result r = Result::More;
    for (int n = 0; n < 140000 && r == Result::More; ++n) {
        memory.transferred = 0;
        r = codec.Advance();
        EXPECT_LE(memory.transferred, 280u);
    }
    return r;
}
Memory Encode(const Sequencer::Project& p) {
    Memory m;
    ProjectFile::Encoder encoder(m.Io(), p);
    EXPECT_EQ(AdvanceAll(encoder, m), Result::Done);
    return m;
}
Result Decode(Memory& m, Sequencer::Project& p) {
    m.position = 0;
    ProjectFile::Decoder decoder(m.Io(), p);
    return AdvanceAll(decoder, m);
}
TEST(ProjectFile, RoundTripSparsePatternsSongsAndSessionSettings) {
    auto p = Example();
    auto m = Encode(*p);
    auto restored = std::make_unique<Sequencer::Project>();
    ASSERT_EQ(Decode(m, *restored), Result::Done);
    EXPECT_STREQ(restored->bank_path, p->bank_path);
    EXPECT_STREQ(restored->tracks[15].instrument_path, p->tracks[15].instrument_path);
    EXPECT_EQ(restored->tracks[15].midi_in, 16);
    EXPECT_EQ(restored->tracks[15].poly_limit, 8);
    EXPECT_EQ(restored->tracks[15].priority, 255);
    EXPECT_FALSE(restored->tracks[15].program_change);
    EXPECT_TRUE(restored->tracks[15].mix.mute);
    EXPECT_FLOAT_EQ(restored->tracks[15].mix.gain, .25f);
    EXPECT_FLOAT_EQ(restored->tracks[15].mix.pan_offset, -1);
    EXPECT_FLOAT_EQ(restored->master_gain, .75f);
    EXPECT_TRUE(restored->quantize);
    EXPECT_FALSE(restored->patterns[0].used);
    EXPECT_EQ(restored->patterns[127].pattern.length, 1);
    EXPECT_EQ(restored->patterns[127].pattern.tracks[15].steps[63].note, 127);
    EXPECT_EQ(restored->patterns[127].pattern.tracks[15].steps[63].param_locks[3].value, 65535);
    EXPECT_EQ(restored->songs[15].tempo_bpm_x100, 14325);
    EXPECT_EQ(restored->songs[15].entries[127].pattern, 127);
    EXPECT_EQ(restored->songs[15].entries[127].repeats, 255);
    EXPECT_LT(sizeof(Sequencer::Project), 3584u * 1024u);
}
TEST(ProjectFile, FullCapacityRoundTripStaysWithinFileAndPerAdvanceBudgets) {
    auto p = Example();
    for (unsigned i = 0; i < Sequencer::kMaxPatterns; ++i) {
        auto& slot = p->patterns[i];
        slot.used = true;
        std::strcpy(slot.name, "Pattern");
        slot.pattern.tracks[15].steps[63].note = static_cast<uint8_t>(i);
    }
    for (auto& song: p->songs) {
        song.used = true;
        std::strcpy(song.name, "Song");
        song.length = Sequencer::kMaxSongEntries;
        for (unsigned i = 0; i < Sequencer::kMaxSongEntries; ++i)
            song.entries[i] = {static_cast<uint8_t>(i), 1};
    }
    p->sample_count = Sequencer::kMaxProjectSamples;
    for (unsigned i = 0; i < p->sample_count; ++i) {
        auto& sample = p->samples[i];
        std::snprintf(sample.path, sizeof(sample.path), "/wavex/samples/%u.wav", i);
        sample.sample_rate = 48000;
        sample.total_frames = 48000;
        sample.channels = 2;
        sample.bits_per_sample = 16;
    }
    auto m = Encode(*p);
    EXPECT_LT(m.bytes.size(), ProjectFile::kMaxFileBytes);
    auto restored = std::make_unique<Sequencer::Project>();
    ASSERT_EQ(Decode(m, *restored), Result::Done);
    for (unsigned i = 0; i < Sequencer::kMaxPatterns; ++i) {
        EXPECT_TRUE(restored->patterns[i].used);
        EXPECT_EQ(restored->patterns[i].pattern.tracks[15].steps[63].note, i);
    }
    EXPECT_EQ(restored->songs[15].entries[127].pattern, 127);
}
TEST(ProjectFile, RejectsMissingPatternReferencesAndInvalidNumbersBeforeWriting) {
    auto p = Example();
    for (int error = 0; error < 3; ++error) {
        if (error == 0)
            p->active_pattern = 128;
        if (error == 1) {
            p->active_pattern = 127;
            p->songs[15].entries[0].pattern = 0;
        }
        if (error == 2) {
            p->songs[15].entries[0].pattern = 127;
            p->master_gain = std::numeric_limits<float>::quiet_NaN();
        }
        Memory m;
        ProjectFile::Encoder encoder(m.Io(), *p);
        EXPECT_EQ(AdvanceAll(encoder, m), Result::Invalid);
        EXPECT_TRUE(m.bytes.empty());
    }
}
TEST(ProjectFile, TruncationAndDuplicateChunksNeverProduceACompleteDocument) {
    auto p = Example();
    const auto original = Encode(*p);
    auto restored = std::make_unique<Sequencer::Project>();
    for (size_t n: {size_t{0}, size_t{11}, size_t{40}, original.bytes.size() - 1}) {
        Memory m = original;
        m.bytes.resize(n);
        EXPECT_NE(Decode(m, *restored), Result::Done);
    }
    Memory duplicate = original;
    duplicate.bytes.insert(duplicate.bytes.end(),
                           original.bytes.begin() + 12,
                           original.bytes.begin() + 12 + 8 + ProjectFile::kHeadBytes);
    EXPECT_EQ(Decode(duplicate, *restored), Result::Invalid);
}
TEST(ProjectFile, UnknownChunksAreSkippedInBoundedSlicesAndFutureMajorIsRejected) {
    auto p = Example();
    auto m = Encode(*p);
    Wxcf::Writer writer(m.Io());
    uint8_t extra[400]{};
    ASSERT_EQ(writer.WriteChunk(0x7000, 0x0200, extra, sizeof(extra)), Wxcf::Result::Ok);
    auto restored = std::make_unique<Sequencer::Project>();
    EXPECT_EQ(Decode(m, *restored), Result::Done);
    m.bytes[7] = 2;
    EXPECT_EQ(Decode(m, *restored), Result::Invalid);
}
TEST(ProjectFile, WriteErrorsAndUnterminatedPathsAreNotSuccessfulSaves) {
    auto p = Example();
    Memory m;
    m.fail_at = 600;
    ProjectFile::Encoder encoder(m.Io(), *p);
    EXPECT_EQ(AdvanceAll(encoder, m), Result::IoError);
    std::memset(p->tracks[15].instrument_path, 'x', sizeof(p->tracks[15].instrument_path));
    Memory bad;
    ProjectFile::Encoder invalid(bad.Io(), *p);
    EXPECT_EQ(AdvanceAll(invalid, bad), Result::Invalid);
}
}  // namespace

namespace {
void AddSample(Sequencer::Project& p) {
    p.sample_count = 1;
    auto& s = p.samples[0];
    std::strcpy(s.path, "/wavex/samples/Long stereo name.wav");
    s.sample_rate = 48000;
    s.total_frames = 48000;
    s.channels = 2;
    s.bits_per_sample = 16;
    s.start_frame = 120;
    s.end_frame = 40000;
    s.loop_start = 500;
    s.loop_end = 20000;
    s.gain_db_x10 = -123;
    s.fade_in_ms = 7;
    s.fade_out_ms = 23;
    s.loop_enabled = true;
    s.channel_mode = Protocol::SAMPLE_CH_MONO_SUM;
}
TEST(ProjectFile, SampleEditsRoundTripWithBoundedTransfersAndNoRuntimeIdentity) {
    auto p = Example();
    AddSample(*p);
    auto m = Encode(*p);
    auto restored = std::make_unique<Sequencer::Project>();
    ASSERT_EQ(Decode(m, *restored), Result::Done);
    ASSERT_EQ(restored->sample_count, 1);
    const auto& s = restored->samples[0];
    EXPECT_STREQ(s.path, p->samples[0].path);
    EXPECT_EQ(s.sample_rate, 48000u);
    EXPECT_EQ(s.total_frames, 48000u);
    EXPECT_EQ(s.start_frame, 120u);
    EXPECT_EQ(s.end_frame, 40000u);
    EXPECT_EQ(s.loop_start, 500u);
    EXPECT_EQ(s.loop_end, 20000u);
    EXPECT_EQ(s.gain_db_x10, -123);
    EXPECT_EQ(s.fade_in_ms, 7);
    EXPECT_EQ(s.fade_out_ms, 23);
    EXPECT_EQ(s.channels, 2);
    EXPECT_EQ(s.bits_per_sample, 16);
    EXPECT_TRUE(s.loop_enabled);
    EXPECT_EQ(s.channel_mode, Protocol::SAMPLE_CH_MONO_SUM);
}
TEST(ProjectFile, SampleTableRequiresCompleteUniqueValidRecords) {
    auto p = Example();
    AddSample(*p);
    const auto good = Encode(*p);
    auto restored = std::make_unique<Sequencer::Project>();
    for (size_t missing: {size_t{1}, size_t{ProjectFile::kSampleBytes + 8}}) {
        auto m = good;
        m.bytes.resize(m.bytes.size() - missing);
        EXPECT_NE(Decode(m, *restored), Result::Done);
    }
    auto duplicate = good;
    duplicate.bytes.insert(
        duplicate.bytes.end(), good.bytes.end() - ProjectFile::kSampleBytes - 8, good.bytes.end());
    EXPECT_EQ(Decode(duplicate, *restored), Result::Invalid);
    p->sample_count = 2;
    p->samples[1] = p->samples[0];
    Memory bad;
    ProjectFile::Encoder duplicates(bad.Io(), *p);
    EXPECT_EQ(AdvanceAll(duplicates, bad), Result::Invalid);
    p->sample_count = 1;
    p->samples[0].end_frame = p->samples[0].start_frame;
    Memory markers;
    ProjectFile::Encoder invalid(markers.Io(), *p);
    EXPECT_EQ(AdvanceAll(invalid, markers), Result::Invalid);
    auto malformed = good;
    malformed.bytes[malformed.bytes.size() - 4] = 2;  // boolean loop_enabled
    EXPECT_EQ(Decode(malformed, *restored), Result::Invalid);
}
TEST(ProjectFile, LegacyProjectHasNoEditsAndNewVersionRequiresSampleCount) {
    auto p = Example();
    auto m = Encode(*p);
    m.bytes.resize(m.bytes.size() - 10);  // remove the empty sample count chunk
    auto restored = std::make_unique<Sequencer::Project>();
    EXPECT_EQ(Decode(m, *restored), Result::Invalid);
    m.bytes[6] = 0;  // version 1.0
    restored->sample_count = 10;
    EXPECT_EQ(Decode(m, *restored), Result::Done);
    EXPECT_EQ(restored->sample_count, 0);
}
}  // namespace
