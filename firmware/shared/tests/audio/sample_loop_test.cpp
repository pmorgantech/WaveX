#include "audio/sample_loop.hpp"

#include <gtest/gtest.h>

#include <array>
using namespace WaveX;
TEST(SampleLoop, LinkedStereoSnapRejectsCancellationAndPreservesBounds) {
    std::array<int16_t, 2048> pcm;
    pcm.fill(1000);
    Protocol::SampleMetadata m;
    m.total_frames = m.end_frame = 1024;
    m.channels = 2;
    m.loop_start = 256;
    m.loop_end = 768;
    // Mono sum crosses through zero, but right does not cross.
    pcm[255 * 2] = -10;
    pcm[256 * 2] = 10;
    pcm[255 * 2 + 1] = 11;
    pcm[256 * 2 + 1] = 9;
    uint32_t result = 999;
    EXPECT_FALSE(SampleLoop::Snap(pcm.data(), m, Protocol::SAMPLE_MARK_LOOP_START, 4, result));
    EXPECT_EQ(result, 999u);
    pcm[255 * 2 + 1] = -11;
    ASSERT_TRUE(SampleLoop::Snap(pcm.data(), m, Protocol::SAMPLE_MARK_LOOP_START, 4, result));
    EXPECT_EQ(result, 256u);
    m.start_frame = 257;
    EXPECT_FALSE(SampleLoop::Snap(pcm.data(), m, Protocol::SAMPLE_MARK_LOOP_START, 0, result));
}
TEST(SampleLoop, CrossfadeIsBoundedAndConvexWithExactEndpoints) {
    EXPECT_EQ(SampleLoop::CrossfadeFrames(20, 48000, 256), 0u);
    EXPECT_EQ(SampleLoop::CrossfadeFrames(20, 48000, 512), 256u);
    EXPECT_EQ(SampleLoop::CrossfadeFrames(20, 192000, 48000), 2048u);
    const auto step = SampleLoop::RampStep(960);
    for (uint32_t i = 0; i < 960; ++i) {
        auto w = SampleLoop::Weight(i, 960, step);
        EXPECT_EQ(SampleLoop::Mix(-32768, -32768, w), -32768);
        EXPECT_EQ(SampleLoop::Mix(32767, 32767, w), 32767);
    }
    EXPECT_EQ(SampleLoop::Mix(-1000, 2000, SampleLoop::Weight(0, 960, step)), -1000);
    EXPECT_EQ(SampleLoop::Mix(-1000, 2000, SampleLoop::Weight(959, 960, step)), 2000);
}
TEST(SampleLoop, StaleEditAndContentCannotSnap) {
    Protocol::SampleMetadata m;
    m.sample_id = 1024;
    m.generation = 3;
    Protocol::SampleSeamRequest r;
    r.expected = SampleLoop::Edit(m);
    r.generation = 3;
    EXPECT_TRUE(SampleLoop::Matches(m, r));
    ++m.loop_start;
    EXPECT_FALSE(SampleLoop::Matches(m, r));
    --m.loop_start;
    ++m.generation;
    EXPECT_FALSE(SampleLoop::Matches(m, r));
}
TEST(SampleLoop, SeamIsMeasuredPerChannelAtExclusiveEnd) {
    std::array<int16_t, 2048> pcm{};
    Protocol::SampleMetadata m;
    m.channels = 2;
    m.sample_rate = 48000;
    m.total_frames = m.end_frame = 1024;
    m.loop_start = 256;
    m.loop_end = 768;
    m.loop_enabled = 1;
    pcm[256 * 2] = 100;
    pcm[256 * 2 + 1] = -200;
    pcm[767 * 2] = -300;
    pcm[767 * 2 + 1] = 400;
    Protocol::SampleSeamStatus s;
    SampleLoop::Measure(pcm.data(), m, s);
    EXPECT_EQ(s.raw_left, 400);
    EXPECT_EQ(s.raw_right, -600);
    EXPECT_EQ(s.left, 400);
    m.loop_crossfade_ms = 20;
    pcm[511 * 2] = 400;
    pcm[512 * 2] = 410;
    SampleLoop::Measure(pcm.data(), m, s);
    EXPECT_EQ(s.left, 10);
    EXPECT_EQ(s.raw_left, 400);
}

TEST(SampleLoop, ForegroundCrossfadeIsIndependentOfReadChunkBoundaries) {
    std::array<int16_t, 512> pcm{}, head{};
    for (unsigned i = 0; i < 512; ++i) {
        pcm[i] = int16_t(10000 - int(i) * 31);
        head[i] = int16_t(-20000 + int(i) * 37);
    }
    auto whole = pcm, split = pcm;
    const uint32_t step = SampleLoop::RampStep(64);
    SampleLoop::ApplyBlock(whole.data(), 256, 0, head.data(), 256, 64, 2, step);
    uint32_t first = 0;
    for (uint32_t chunk: {191u, 1u, 1u, 16u, 1u, 45u, 1u}) {
        SampleLoop::ApplyBlock(
            split.data() + first * 2, chunk, first, head.data(), 256, 64, 2, step);
        first += chunk;
    }
    EXPECT_EQ(first, 256u);
    EXPECT_EQ(split, whole);
    EXPECT_EQ(whole[382], pcm[382]);
    EXPECT_EQ(whole[510], head[126]);
    EXPECT_EQ(whole[511], head[127]);
}

TEST(SampleLoop, TrimPastAnInactiveLoopKeepsResolvableBounds) {
    Protocol::SampleMetadata m;
    m.total_frames = 1024;
    m.start_frame = 800;
    m.end_frame = 1000;
    m.loop_start = 100;
    m.loop_end = 500;
    m.Resolve();
    EXPECT_EQ(m.loop_start, 800u);
    EXPECT_EQ(m.loop_end, 1000u);
    EXPECT_LT(m.loop_start, m.loop_end);
}
