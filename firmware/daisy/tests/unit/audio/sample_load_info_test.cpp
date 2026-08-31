#include "audio/sample_load_info.hpp"

#include <gtest/gtest.h>

using WaveX::AudioEngine::BuildResidentSampleInfo;
using WaveX::AudioEngine::ResidentSampleInfo;
using WaveX::Protocol::SampleLoadMessage;
using WaveX::Wav::WavInfo;

namespace {

constexpr uint32_t kArenaBytes = 60u * 1024u * 1024u;

WavInfo ValidWav(uint32_t sample_rate = 48000,
                 uint16_t channels = 2,
                 uint16_t bits = 16,
                 uint32_t data_size = 48000u * 2u * 2u) {
    WavInfo wav;
    wav.audio_format = 1;
    wav.num_channels = channels;
    wav.sample_rate = sample_rate;
    wav.bits_per_sample = bits;
    wav.data_offset = 44;
    wav.data_size = data_size;
    return wav;
}

SampleLoadMessage Request(uint16_t rate = 48000, uint8_t channels = 2, uint8_t bits = 16) {
    return SampleLoadMessage(7, 0, rate, channels, bits, "/samples/test.wav");
}

}  // namespace

TEST(SampleLoadInfoTest, UsesParsedGeometryInsteadOfOptionalWireHints) {
    const WavInfo wav = ValidWav(/*sample_rate=*/96000, /*channels=*/1, /*bits=*/16, 192000);
    const SampleLoadMessage request = Request(/*rate hint cannot represent 96k=*/0,
                                              /*stale channels=*/2,
                                              /*stale bits=*/24);
    ResidentSampleInfo info;

    ASSERT_TRUE(
        BuildResidentSampleInfo(request, wav, wav.data_offset + wav.data_size, kArenaBytes, info));
    EXPECT_EQ(info.sample_id, request.sample_id);
    EXPECT_EQ(info.sample_rate, 96000u);
    EXPECT_EQ(info.channels, 1u);
    EXPECT_EQ(info.bit_depth, 16u);
    EXPECT_EQ(info.total_frames, 96000u);
}

TEST(SampleLoadInfoTest, RejectsPcm24ThatTheResidentVoicePathCannotPlay) {
    const WavInfo wav = ValidWav(/*sample_rate=*/48000, /*channels=*/1, /*bits=*/24, 144000);
    ResidentSampleInfo info;

    EXPECT_FALSE(BuildResidentSampleInfo(
        Request(), wav, wav.data_offset + wav.data_size, kArenaBytes, info));
}

TEST(SampleLoadInfoTest, RejectsEmptyDataBeforeAnyEvictionCanRun) {
    WavInfo wav = ValidWav();
    wav.data_size = 0;
    ResidentSampleInfo info;
    EXPECT_FALSE(BuildResidentSampleInfo(Request(), wav, wav.data_offset, kArenaBytes, info));
}

TEST(SampleLoadInfoTest, RejectsDataChunkLargerThanResidentArena) {
    WavInfo wav = ValidWav();
    wav.data_size = kArenaBytes + 4;
    ResidentSampleInfo info;
    EXPECT_FALSE(BuildResidentSampleInfo(
        Request(), wav, wav.data_offset + wav.data_size, kArenaBytes, info));
}

TEST(SampleLoadInfoTest, RejectsDataChunkThatRunsPastEndOfFile) {
    const WavInfo wav = ValidWav();
    ResidentSampleInfo info;
    EXPECT_FALSE(BuildResidentSampleInfo(
        Request(), wav, wav.data_offset + wav.data_size - 1, kArenaBytes, info));
}

TEST(SampleLoadInfoTest, RejectsZeroSampleRateAndPartialFrames) {
    WavInfo wav = ValidWav();
    ResidentSampleInfo info;

    wav.sample_rate = 0;
    EXPECT_FALSE(BuildResidentSampleInfo(
        Request(), wav, wav.data_offset + wav.data_size, kArenaBytes, info));

    wav = ValidWav();
    ++wav.data_size;
    EXPECT_FALSE(BuildResidentSampleInfo(
        Request(), wav, wav.data_offset + wav.data_size, kArenaBytes, info));
}
