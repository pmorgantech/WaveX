// Host tests for the streaming resampler (src/audio/linear_resampler.hpp).
//
// This code had no coverage while being the subject of three separate
// playback-stall fixes in 2026-08, all of them edge cases in how many frames
// a pass could consume or produce. The output-count and short-input cases
// below are the regression net for that family of bug; CmsisReference pins
// the arithmetic against the CMSIS-DSP function this replaced.

#include "linear_resampler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using WaveX::AudioEngine::LinearResampleOutputFrames;
using WaveX::AudioEngine::ResampleChannel;
using WaveX::AudioEngine::ResampleInterleaved;
using WaveX::AudioEngine::ResampleStreamInterleaved;
using WaveX::AudioEngine::StreamResamplerState;

namespace {

// The two rates that matter here: WAV content at 44.1 kHz on a 48 kHz engine
// (upsampling, the case that deadlocked), and 96 kHz content (downsampling,
// where the input cap works differently).
constexpr float kRatio44kTo48k = 48000.0f / 44100.0f;
constexpr float kRatio96kTo48k = 48000.0f / 96000.0f;

// Verbatim reimplementation of CMSIS-DSP arm_linear_interp_q15() - the
// function the header replaced - so the tests can assert the new arithmetic
// is bit-identical rather than merely close. Input x is 12.20 fixed point.
int16_t CmsisLinearInterpQ15(const int16_t* table, int32_t x, uint32_t n_values) {
    int32_t index = ((x & (int32_t)0xFFF00000) >> 20);
    if (index >= (int32_t)(n_values - 1)) {
        return table[n_values - 1];
    }
    if (index < 0) {
        return table[0];
    }
    const int32_t fract = (x & 0x000FFFFF);
    const int16_t y0 = table[index];
    const int16_t y1 = table[index + 1];
    int64_t y = ((int64_t)y0 * (0xFFFFF - fract));
    y += ((int64_t)y1 * fract);
    return (int16_t)(y >> 20);
}

// Interleaves `channels` independent ramps, each offset so channels are
// trivially distinguishable.
std::vector<int16_t> MakeInterleavedRamps(uint32_t frames, uint32_t channels) {
    std::vector<int16_t> buf(static_cast<size_t>(frames) * channels);
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            buf[static_cast<size_t>(f) * channels + ch] = static_cast<int16_t>(f * 4 + ch * 1000);
        }
    }
    return buf;
}

}  // namespace

// --- Guard conditions --------------------------------------------------
// Fewer than two input frames cannot be interpolated. Callers treat a 0
// return as "discard this pass"; when a caller then retried the same
// too-short request forever, playback stalled into permanent underrun.

TEST(LinearResamplerGuards, ReturnsZeroBelowTwoInputFrames) {
    std::vector<int16_t> src(8, 1234);
    std::vector<int16_t> dst(64, 0);

    EXPECT_EQ(ResampleInterleaved(src.data(), 0, dst.data(), 2, kRatio44kTo48k), 0u);
    EXPECT_EQ(ResampleInterleaved(src.data(), 1, dst.data(), 2, kRatio44kTo48k), 0u);
    EXPECT_EQ(LinearResampleOutputFrames(0, kRatio44kTo48k), 0u);
    EXPECT_EQ(LinearResampleOutputFrames(1, kRatio44kTo48k), 0u);

    // Two frames is the smallest workable input.
    EXPECT_GT(ResampleInterleaved(src.data(), 2, dst.data(), 2, kRatio44kTo48k), 0u);
}

TEST(LinearResamplerGuards, RejectsNonPositiveRatioAndNullArguments) {
    std::vector<int16_t> src(64, 7);
    std::vector<int16_t> dst(64, 0);

    EXPECT_EQ(ResampleInterleaved(src.data(), 16, dst.data(), 2, 0.0f), 0u);
    EXPECT_EQ(ResampleInterleaved(src.data(), 16, dst.data(), 2, -1.5f), 0u);
    EXPECT_EQ(ResampleInterleaved(nullptr, 16, dst.data(), 2, kRatio44kTo48k), 0u);
    EXPECT_EQ(ResampleInterleaved(src.data(), 16, nullptr, 2, kRatio44kTo48k), 0u);
    EXPECT_EQ(ResampleInterleaved(src.data(), 16, dst.data(), 0, kRatio44kTo48k), 0u);
}

// --- Output sizing -----------------------------------------------------
// Callers size the destination from LinearResampleOutputFrames() before
// committing. If it disagreed with what the resampler writes, a pass would
// either overrun the destination or leave an uninitialised tail.

TEST(LinearResamplerSizing, PredictedCountMatchesFramesWritten) {
    const uint32_t kChannels = 2;
    for (uint32_t frames: {2u, 3u, 17u, 256u, 1023u, 1879u}) {
        for (float ratio: {kRatio44kTo48k, kRatio96kTo48k, 1.0f, 2.0f}) {
            const uint32_t predicted = LinearResampleOutputFrames(frames, ratio);
            std::vector<int16_t> src = MakeInterleavedRamps(frames, kChannels);
            std::vector<int16_t> dst(static_cast<size_t>(predicted + 8) * kChannels, -31337);

            const uint32_t written =
                ResampleInterleaved(src.data(), frames, dst.data(), kChannels, ratio);
            EXPECT_EQ(written, predicted) << "frames=" << frames << " ratio=" << ratio;

            // Everything the caller was told to expect is initialised, and
            // nothing beyond it was touched.
            for (uint32_t f = 0; f < written; ++f) {
                for (uint32_t ch = 0; ch < kChannels; ++ch) {
                    EXPECT_NE(dst[static_cast<size_t>(f) * kChannels + ch], -31337)
                        << "frames=" << frames << " ratio=" << ratio << " f=" << f;
                }
            }
            EXPECT_EQ(dst[static_cast<size_t>(written) * kChannels], -31337);
        }
    }
}

TEST(LinearResamplerSizing, UpsamplingProducesMoreFramesAndDownsamplingFewer) {
    const uint32_t frames = 441;
    EXPECT_GT(LinearResampleOutputFrames(frames, kRatio44kTo48k), frames - 1);
    EXPECT_LT(LinearResampleOutputFrames(frames, kRatio96kTo48k), frames);
}

// --- Channel handling --------------------------------------------------
// Mono, stereo and the Stage B 8-channel voice layout all run through the
// same per-channel core; the wrapper must keep them independent and aligned.

TEST(LinearResamplerChannels, HandlesMonoStereoAndEightChannels) {
    const uint32_t frames = 128;
    for (uint32_t channels: {1u, 2u, 8u}) {
        std::vector<int16_t> src = MakeInterleavedRamps(frames, channels);
        const uint32_t expected = LinearResampleOutputFrames(frames, kRatio44kTo48k);
        std::vector<int16_t> dst(static_cast<size_t>(expected) * channels, 0);

        const uint32_t written =
            ResampleInterleaved(src.data(), frames, dst.data(), channels, kRatio44kTo48k);
        ASSERT_EQ(written, expected) << "channels=" << channels;

        // Each channel must carry its own ramp, not a neighbour's. The
        // ramps are offset by 1000 per channel, so a crossed channel shows
        // up immediately.
        for (uint32_t ch = 0; ch < channels; ++ch) {
            EXPECT_NEAR(dst[ch], static_cast<int16_t>(ch * 1000), 2) << "channels=" << channels;
            const int16_t last = dst[static_cast<size_t>(written - 1) * channels + ch];
            EXPECT_GT(last, static_cast<int16_t>(ch * 1000)) << "channels=" << channels;
        }
    }
}

TEST(LinearResamplerChannels, ChannelResultIsIndependentOfChannelCount) {
    // Channel 0 of a stereo pass and of an 8-channel pass must resample
    // identically - the core reads through a stride and nothing else.
    const uint32_t frames = 200;
    const uint32_t expected = LinearResampleOutputFrames(frames, kRatio44kTo48k);

    std::vector<int16_t> src2 = MakeInterleavedRamps(frames, 2);
    std::vector<int16_t> dst2(static_cast<size_t>(expected) * 2, 0);
    ASSERT_EQ(ResampleInterleaved(src2.data(), frames, dst2.data(), 2, kRatio44kTo48k), expected);

    std::vector<int16_t> src8 = MakeInterleavedRamps(frames, 8);
    std::vector<int16_t> dst8(static_cast<size_t>(expected) * 8, 0);
    ASSERT_EQ(ResampleInterleaved(src8.data(), frames, dst8.data(), 8, kRatio44kTo48k), expected);

    for (uint32_t f = 0; f < expected; ++f) {
        EXPECT_EQ(dst2[static_cast<size_t>(f) * 2], dst8[static_cast<size_t>(f) * 8])
            << "frame " << f;
    }
}

TEST(LinearResamplerChannels, SingleChannelCoreHonoursArbitraryStrides) {
    // Dispatching one channel at a time is the intended use for callers that
    // want to drive channels individually.
    const uint32_t frames = 64;
    const uint32_t channels = 8;
    std::vector<int16_t> src = MakeInterleavedRamps(frames, channels);
    const uint32_t expected = LinearResampleOutputFrames(frames, kRatio44kTo48k);

    std::vector<int16_t> interleaved(static_cast<size_t>(expected) * channels, 0);
    ASSERT_EQ(ResampleInterleaved(src.data(), frames, interleaved.data(), channels, kRatio44kTo48k),
              expected);

    for (uint32_t ch = 0; ch < channels; ++ch) {
        // Same channel, resampled straight into a contiguous buffer.
        std::vector<int16_t> contiguous(expected, 0);
        const uint32_t written = ResampleChannel(
            src.data() + ch, frames, channels, contiguous.data(), 1, kRatio44kTo48k);
        ASSERT_EQ(written, expected);
        for (uint32_t f = 0; f < expected; ++f) {
            EXPECT_EQ(contiguous[f], interleaved[static_cast<size_t>(f) * channels + ch])
                << "ch=" << ch << " f=" << f;
        }
    }
}

// --- Arithmetic --------------------------------------------------------

// The interpolator weights are (0xFFFFF - fract) and fract, which sum to
// 2^20 - 1 rather than 2^20, so every output is scaled by 1048575/1048576 and
// then floored: at exact sample positions the result is src - 1, not src.
// That is inherited verbatim from CMSIS arm_linear_interp_q15 (see
// MatchesCmsisLinearInterpBitForBit) and is preserved deliberately - this
// extraction is a refactor, not a DSP change. It is a ~-120 dBFS error,
// inaudible at 16 bits, but it does mean unity-ratio resampling is not
// bit-transparent. Worth correcting when the polyphase rewrite lands
// (docs/roadmap.md Phase 5), where the weights get rebuilt anyway.
TEST(LinearResamplerMath, UnityRatioReproducesSamplesWithinOneLsb) {
    const uint32_t frames = 64;
    const uint32_t channels = 2;
    std::vector<int16_t> src = MakeInterleavedRamps(frames, channels);
    const uint32_t expected = LinearResampleOutputFrames(frames, 1.0f);
    std::vector<int16_t> dst(static_cast<size_t>(expected) * channels, 0);

    ASSERT_EQ(ResampleInterleaved(src.data(), frames, dst.data(), channels, 1.0f), expected);
    for (uint32_t f = 0; f < expected; ++f) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            EXPECT_NEAR(dst[static_cast<size_t>(f) * channels + ch],
                        src[static_cast<size_t>(f) * channels + ch],
                        1)
                << "f=" << f << " ch=" << ch;
        }
    }
}

TEST(LinearResamplerMath, HalvingRatioInterpolatesMidpoints) {
    // ratio 0.5 steps two input frames per output frame, so a ramp stays a
    // ramp of twice the slope - a wrong step shows up as a pitch error.
    const uint32_t frames = 64;
    std::vector<int16_t> src(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        src[i] = static_cast<int16_t>(i * 100);
    }
    const uint32_t expected = LinearResampleOutputFrames(frames, 0.5f);
    std::vector<int16_t> dst(expected, 0);

    ASSERT_EQ(ResampleChannel(src.data(), frames, 1, dst.data(), 1, 0.5f), expected);
    for (uint32_t f = 0; f < expected; ++f) {
        EXPECT_NEAR(dst[f], static_cast<int16_t>(f * 200), 1) << "f=" << f;
    }
}

TEST(LinearResamplerMath, MatchesCmsisLinearInterpBitForBit) {
    // Within the range the CMSIS 12.20 index could represent, the extracted
    // arithmetic must be identical to what shipped before.
    const uint32_t frames = 512;
    std::vector<int16_t> src(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        // Something less regular than a ramp, and signed on both sides.
        src[i] = static_cast<int16_t>(((i * 2731) % 60000) - 30000);
    }
    const uint32_t expected = LinearResampleOutputFrames(frames, kRatio44kTo48k);
    std::vector<int16_t> dst(expected, 0);
    ASSERT_EQ(ResampleChannel(src.data(), frames, 1, dst.data(), 1, kRatio44kTo48k), expected);

    float position = 0.0f;
    const float step = 1.0f / kRatio44kTo48k;
    for (uint32_t f = 0; f < expected; ++f) {
        const int32_t x_q31 = static_cast<int32_t>(position * 1048576.0f);
        EXPECT_EQ(dst[f], CmsisLinearInterpQ15(src.data(), x_q31, frames)) << "f=" << f;
        position += step;
    }
}

TEST(LinearResamplerMath, LongSourcesDoNotCollapseToTheFirstSample) {
    // CMSIS packed the index into a signed 12.20 word, so a position past
    // 2047 overflowed the sign bit and every remaining output silently became
    // the FIRST sample - a DC level, no error reported. Current callers are
    // bounded below that by ring capacity, so this pins a limit that no
    // longer exists rather than a bug that was reachable.
    const uint32_t frames = 4096;
    std::vector<int16_t> src(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        src[i] = static_cast<int16_t>((i % 2000) - 1000);
    }
    const uint32_t expected = LinearResampleOutputFrames(frames, 1.0f);
    std::vector<int16_t> dst(expected, 0);
    ASSERT_EQ(ResampleChannel(src.data(), frames, 1, dst.data(), 1, 1.0f), expected);
    ASSERT_GT(expected, 3000u);

    // At unity the output tracks the input all the way out, well past 2047
    // (within the 1 LSB the weighting costs - see the unity-ratio test).
    for (uint32_t f = 2048; f < expected; ++f) {
        EXPECT_NEAR(dst[f], src[f], 1) << "f=" << f;
    }
    // And specifically is not pinned to src[0].
    EXPECT_NE(dst[3000], src[0]);
}

TEST(LinearResamplerMath, ExactPositionsLoseExactlyOneLsb) {
    // Pins the quirk described above so a future weighting change has to be
    // deliberate: at integer positions the output is floor(src * (2^20-1) /
    // 2^20), i.e. src - 1 for any non-zero sample.
    std::vector<int16_t> src = {1000, 2000, 3000, 4000};
    std::vector<int16_t> dst(4, 0);
    const uint32_t written = ResampleChannel(src.data(), 4, 1, dst.data(), 1, 1.0f);
    ASSERT_EQ(written, 3u);
    EXPECT_EQ(dst[0], 999);
    EXPECT_EQ(dst[1], 1999);
    EXPECT_EQ(dst[2], 2999);
}

// --- Streaming continuity ----------------------------------------------
// The stateless path restarts at phase 0 every call, so feeding it a stream in
// chunks discards up to a frame and jumps phase at each boundary - an audible
// warble at the ~42 Hz chunk rate. These pin the stateful path against that.

TEST(LinearResamplerStream, ChunkedMatchesWholeBufferOnARamp) {
    // A ramp resampled in one pass and in chunks must agree: any phase reset
    // or dropped frame at a boundary shows up as a step in the difference.
    const uint32_t total = 600;
    const uint32_t channels = 1;
    std::vector<int16_t> src(total);
    for (uint32_t i = 0; i < total; ++i) {
        src[i] = static_cast<int16_t>(i * 20);
    }

    StreamResamplerState st;
    std::vector<int16_t> chunked;
    const uint32_t chunk = 64;
    for (uint32_t off = 0; off < total; off += chunk) {
        const uint32_t n = std::min(chunk, total - off);
        std::vector<int16_t> out(n * 4 + 8, 0);
        const uint32_t got = ResampleStreamInterleaved(st,
                                                       src.data() + off,
                                                       n,
                                                       out.data(),
                                                       static_cast<uint32_t>(out.size()),
                                                       channels,
                                                       kRatio44kTo48k);
        chunked.insert(chunked.end(), out.begin(), out.begin() + got);
    }

    // On a ramp with a constant step the output is itself a ramp. Every
    // adjacent difference must be the same, to within rounding - a boundary
    // glitch appears as one interval far from the rest.
    ASSERT_GT(chunked.size(), 400u);
    const int expected_step = chunked[201] - chunked[200];
    for (size_t i = 5; i + 5 < chunked.size(); ++i) {
        const int d = chunked[i + 1] - chunked[i];
        EXPECT_NEAR(d, expected_step, 2) << "discontinuity at output frame " << i;
    }
}

TEST(LinearResamplerStream, PhaseAdvancesAcrossChunkBoundaries) {
    // Total output for a chunked stream must track the ratio. Losing a frame
    // per chunk - the old behaviour - shows up as a deficit that grows with
    // the number of chunks.
    const uint32_t total = 4410;
    std::vector<int16_t> src(total, 0);
    for (uint32_t i = 0; i < total; ++i) {
        src[i] = static_cast<int16_t>((i % 100) * 300 - 15000);
    }

    StreamResamplerState st;
    uint32_t produced = 0;
    const uint32_t chunk = 147;  // 30 chunks: a lost frame each would be obvious
    for (uint32_t off = 0; off < total; off += chunk) {
        const uint32_t n = std::min(chunk, total - off);
        std::vector<int16_t> out(n * 2 + 8, 0);
        produced += ResampleStreamInterleaved(st,
                                              src.data() + off,
                                              n,
                                              out.data(),
                                              static_cast<uint32_t>(out.size()),
                                              1,
                                              kRatio44kTo48k);
    }

    // 4410 in at 44.1 -> 48 kHz is 4800 out. Allow a couple of frames of
    // start-up slack, but nothing like the 30 the old path would have lost.
    const uint32_t ideal = static_cast<uint32_t>(total * kRatio44kTo48k);
    EXPECT_NEAR(produced, ideal, 3u);
}

TEST(LinearResamplerStream, HandlesStereoAndEightChannelStreams) {
    for (uint32_t channels: {2u, 8u}) {
        StreamResamplerState st;
        const uint32_t chunk = 128;
        std::vector<int16_t> src = MakeInterleavedRamps(chunk, channels);
        std::vector<int16_t> out(chunk * channels * 2, 0);

        const uint32_t first = ResampleStreamInterleaved(
            st, src.data(), chunk, out.data(), chunk * 2, channels, kRatio44kTo48k);
        ASSERT_GT(first, 0u) << "channels=" << channels;
        EXPECT_TRUE(st.has_history);
        // History must be the LAST frame of the chunk, per channel.
        for (uint32_t ch = 0; ch < channels; ++ch) {
            EXPECT_EQ(st.history[ch], src[static_cast<size_t>(chunk - 1) * channels + ch])
                << "channels=" << channels << " ch=" << ch;
        }
        // Phase is rebased onto the next chunk, so it lands just below zero.
        EXPECT_GT(st.phase, -1.0f) << "channels=" << channels;
        EXPECT_LE(st.phase, 0.0f) << "channels=" << channels;
    }
}

TEST(LinearResamplerStream, ResetClearsContinuity) {
    std::vector<int16_t> src(64, 1234);
    std::vector<int16_t> out(256, 0);
    StreamResamplerState st;
    ResampleStreamInterleaved(st, src.data(), 64, out.data(), 256, 1, kRatio44kTo48k);
    ASSERT_TRUE(st.has_history);
    st.Reset();
    EXPECT_FALSE(st.has_history);
    EXPECT_EQ(st.phase, 0.0f);
}
