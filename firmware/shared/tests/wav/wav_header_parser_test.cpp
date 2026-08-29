// Host tests for the shared WAV header parser (wav/wav_header_parser.hpp),
// replacing three hand-rolled chunk walks (review M12). The odd-chunk
// padding cases are the regression the extraction exists for: the
// audio_engine copies skipped odd chunks without the RIFF pad byte and
// mis-parsed any WAV with an odd-length LIST/INFO chunk before data.

#include "wav_header_parser.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace WaveX::Wav;

namespace {

void PushLe16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void PushLe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 24) & 0xFF);
}
void PushTag(std::vector<uint8_t>& v, const char* tag) {
    v.insert(v.end(), tag, tag + 4);
}

std::vector<uint8_t> RiffHeader() {
    std::vector<uint8_t> v;
    PushTag(v, "RIFF");
    PushLe32(v, 0);  // riff size: parser doesn't rely on it
    PushTag(v, "WAVE");
    return v;
}

void PushFmt(std::vector<uint8_t>& v,
             uint16_t fmt = 1,
             uint16_t ch = 2,
             uint32_t sr = 48000,
             uint16_t bits = 16,
             uint16_t extra_bytes = 0) {
    PushTag(v, "fmt ");
    PushLe32(v, 16 + extra_bytes);
    PushLe16(v, fmt);
    PushLe16(v, ch);
    PushLe32(v, sr);
    PushLe32(v, sr * ch * bits / 8);  // byte rate
    PushLe16(v, ch * bits / 8);       // block align
    PushLe16(v, bits);
    for (uint16_t i = 0; i < extra_bytes; ++i) {
        v.push_back(0xEE);
    }
    if ((16 + extra_bytes) & 1) {
        v.push_back(0);  // pad byte
    }
}

void PushChunk(std::vector<uint8_t>& v, const char* tag, uint32_t size, uint8_t fill = 0x11) {
    PushTag(v, tag);
    PushLe32(v, size);
    for (uint32_t i = 0; i < size; ++i) {
        v.push_back(fill);
    }
    if (size & 1) {
        v.push_back(0);  // RIFF pad byte for odd chunk
    }
}

void PushData(std::vector<uint8_t>& v, uint32_t size) {
    PushTag(v, "data");
    PushLe32(v, size);
    for (uint32_t i = 0; i < size; ++i) {
        v.push_back(0x22);
    }
}

TEST(WavHeaderParserTest, CanonicalHeaderParses) {
    auto wav = RiffHeader();
    PushFmt(wav);
    const uint32_t expect_offset = static_cast<uint32_t>(wav.size()) + 8;
    PushData(wav, 96);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    ASSERT_EQ(ParseWavHeader(r, info), ParseResult::Ok);
    EXPECT_EQ(info.audio_format, 1);
    EXPECT_EQ(info.num_channels, 2);
    EXPECT_EQ(info.sample_rate, 48000u);
    EXPECT_EQ(info.bits_per_sample, 16);
    EXPECT_EQ(info.data_offset, expect_offset);
    EXPECT_EQ(info.data_size, 96u);
}

TEST(WavHeaderParserTest, FmtWithExtensionBytesIsSkippedCorrectly) {
    auto wav = RiffHeader();
    PushFmt(wav, 1, 1, 44100, 24, /*extra=*/2);  // csz = 18
    const uint32_t expect_offset = static_cast<uint32_t>(wav.size()) + 8;
    PushData(wav, 10);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    ASSERT_EQ(ParseWavHeader(r, info), ParseResult::Ok);
    EXPECT_EQ(info.sample_rate, 44100u);
    EXPECT_EQ(info.bits_per_sample, 24);
    EXPECT_EQ(info.data_offset, expect_offset);
}

// The M12 regression: an odd-sized chunk before data. Without the RIFF pad
// byte the walk lands one byte off and never finds the data chunk.
TEST(WavHeaderParserTest, OddSizedChunkBeforeDataIsPadded) {
    auto wav = RiffHeader();
    PushFmt(wav);
    PushChunk(wav, "LIST", 7);  // odd size -> 1 pad byte on the wire
    const uint32_t expect_offset = static_cast<uint32_t>(wav.size()) + 8;
    PushData(wav, 4);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    ASSERT_EQ(ParseWavHeader(r, info), ParseResult::Ok);
    EXPECT_EQ(info.data_offset, expect_offset);
    EXPECT_EQ(info.data_size, 4u);
}

TEST(WavHeaderParserTest, OddFmtExtensionIsPadded) {
    auto wav = RiffHeader();
    PushFmt(wav, 1, 2, 48000, 16, /*extra=*/1);  // csz = 17, padded
    const uint32_t expect_offset = static_cast<uint32_t>(wav.size()) + 8;
    PushData(wav, 4);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    ASSERT_EQ(ParseWavHeader(r, info), ParseResult::Ok);
    EXPECT_EQ(info.data_offset, expect_offset);
}

TEST(WavHeaderParserTest, DataBeforeFmtIsTolerated) {
    auto wav = RiffHeader();
    PushData(wav, 8);
    PushFmt(wav, 1, 1, 22050, 16);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    ASSERT_EQ(ParseWavHeader(r, info), ParseResult::Ok);
    EXPECT_EQ(info.sample_rate, 22050u);
    EXPECT_EQ(info.data_size, 8u);
}

TEST(WavHeaderParserTest, MissingDataReportsButKeepsFmtFields) {
    auto wav = RiffHeader();
    PushFmt(wav, 1, 2, 96000, 24);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    EXPECT_EQ(ParseWavHeader(r, info), ParseResult::NoDataChunk);
    // Callers that only need format metadata (browse pages) can still use it.
    EXPECT_EQ(info.sample_rate, 96000u);
    EXPECT_EQ(info.num_channels, 2);
}

TEST(WavHeaderParserTest, NotRiffRejected) {
    std::vector<uint8_t> junk(64, 0xAB);
    MemReader r(junk.data(), junk.size());
    WavInfo info;
    EXPECT_EQ(ParseWavHeader(r, info), ParseResult::NotRiffWave);
}

TEST(WavHeaderParserTest, TruncatedFileReported) {
    std::vector<uint8_t> tiny = {'R', 'I', 'F', 'F'};
    MemReader r(tiny.data(), tiny.size());
    WavInfo info;
    EXPECT_EQ(ParseWavHeader(r, info), ParseResult::IoError);
}

TEST(WavHeaderParserTest, UndersizedFmtChunkRejected) {
    auto wav = RiffHeader();
    PushTag(wav, "fmt ");
    PushLe32(wav, 8);  // < 16 mandatory bytes
    for (int i = 0; i < 8; ++i)
        wav.push_back(0);

    MemReader r(wav.data(), wav.size());
    WavInfo info;
    EXPECT_EQ(ParseWavHeader(r, info), ParseResult::BadFmtChunk);
}

TEST(WavHeaderParserTest, ChunkFloodTerminates) {
    // >64 zero-size chunks: the bounded walk must give up cleanly rather
    // than hang on a degenerate file.
    auto wav = RiffHeader();
    for (int i = 0; i < 100; ++i) {
        PushChunk(wav, "JUNK", 0);
    }
    MemReader r(wav.data(), wav.size());
    WavInfo info;
    EXPECT_NE(ParseWavHeader(r, info), ParseResult::Ok);
}

}  // namespace

// Duration is computed from data_size, and the obvious 32-bit multiply
// overflows above 4,294,967 frames - 97.4 s at 44.1 kHz. Every fixture above
// is short, which is exactly why that bug reached hardware: three-minute
// songs listed as 38 seconds. These cases are all longer than the wrap point.
TEST(WavDurationTest, ShortFileIsExact) {
    WaveX::Wav::WavInfo info;
    info.sample_rate = 44100;
    info.num_channels = 2;
    info.bits_per_sample = 16;
    info.data_size = 44100 * 4 * 5;  // 5.000 s
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 5000u);
}

TEST(WavDurationTest, SurvivesThePointWhere32BitWraps) {
    WaveX::Wav::WavInfo info;
    info.sample_rate = 44100;
    info.num_channels = 2;
    info.bits_per_sample = 16;

    // 97.4 s is where frames * 1000 exceeds UINT32_MAX. Straddle it.
    info.data_size = static_cast<uint32_t>(97ull * 44100 * 4);
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 97000u);
    info.data_size = static_cast<uint32_t>(98ull * 44100 * 4);
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 98000u);
}

TEST(WavDurationTest, ThreeMinuteFileAt44k) {
    WaveX::Wav::WavInfo info;
    info.sample_rate = 44100;
    info.num_channels = 2;
    info.bits_per_sample = 16;
    info.data_size = static_cast<uint32_t>(180ull * 44100 * 4);
    // The 32-bit form reported 82608 ms here.
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 180000u);
}

TEST(WavDurationTest, TenMinuteFileAt48k) {
    WaveX::Wav::WavInfo info;
    info.sample_rate = 48000;
    info.num_channels = 2;
    info.bits_per_sample = 24;
    info.data_size = static_cast<uint32_t>(600ull * 48000 * 6);
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 600000u);
}

TEST(WavDurationTest, MonoAnd8BitAreNotSpecialCased) {
    WaveX::Wav::WavInfo info;
    info.sample_rate = 22050;
    info.num_channels = 1;
    info.bits_per_sample = 8;
    info.data_size = 22050 * 300;  // 300 s
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 300000u);
}

// Undeterminable rather than a divide-by-zero or a nonsense figure.
TEST(WavDurationTest, ReturnsZeroWhenFormatIsUnknown) {
    WaveX::Wav::WavInfo info;
    info.data_size = 1000000;
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 0u);

    info.sample_rate = 44100;
    info.num_channels = 0;
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 0u);

    info.num_channels = 2;
    info.bits_per_sample = 4;  // below one whole byte per sample
    EXPECT_EQ(WaveX::Wav::DurationMs(info), 0u);
}
