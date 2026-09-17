#include "audio/playback_cursor.hpp"

#include <gtest/gtest.h>

#include "audio/linear_resampler.hpp"
#include <array>
using namespace WaveX::AudioEngine;

TEST(PlaybackCursor, LongFileRetainsAdjacentFramesAndLoopHistory) {
    SourceFrameWalk native{0xf0000000, 0, 0, 1, false};
    EXPECT_EQ(native.Next(), 0xf0000000u);
    EXPECT_EQ(native.Next(), 0xf0000001u);
    SourceFrameWalk rewind{100, 999, -0.5f, 0.5f, false};
    EXPECT_EQ(rewind.Next(), 999u);
    EXPECT_EQ(rewind.Next(), 100u);
    EXPECT_EQ(rewind.Next(), 100u);
    EXPECT_EQ(rewind.Next(), 101u);
    SourceFrameWalk gap;
    EXPECT_EQ(gap.Next(), SourceFrameWalk::kSilent);
}

TEST(PlaybackCursor, TagsFollowResamplerPhaseAcrossChunksAtBothRates) {
    for (float ratio: {48000.0f / 44100.0f, 0.5f}) {
        StreamResamplerState resampler;
        uint32_t history = SourceFrameWalk::kSilent;
        std::array<int16_t, 64> input{};
        std::array<int16_t, 256> output{};
        for (uint32_t first = 0; first < 256; first += 64) {
            for (uint32_t i = 0; i < 64; ++i)
                input[i] = static_cast<int16_t>((first + i) * 64);
            SourceFrameWalk walk{
                first, history, resampler.has_history ? resampler.phase : 0, 1.0f / ratio, false};
            const auto count = ResampleStreamInterleaved(
                resampler, input.data(), 64, output.data(), 256, 1, ratio);
            ASSERT_GT(count, 0u);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t tag = walk.Next();
                // Ramp interpolation lies between this frame and its next.
                EXPECT_GE(output[i], static_cast<int32_t>(tag * 64) - 1);
                EXPECT_LE(output[i], static_cast<int32_t>((tag + 1) * 64));
            }
            history = first + 63;
        }
    }
}

TEST(PlaybackCursor, NewestVoiceSecondaryLoopAndStop) {
    std::array<int16_t, 256> pcm{}, other{};
    VoiceManager manager;
    manager.Init(48000);
    VoiceTriggerParams params;
    params.sample = pcm.data();
    params.sample_frames = 256;
    params.start_frame = 20;
    params.end_frame = 100;
    params.loop = true;
    params.loop_start = 30;
    params.loop_end = 50;
    params.attack_s = 0;
    params.decay_s = 0;
    params.sustain_level = 1;
    params.release_s = 0;
    params.note = params.root_note = 60;
    params.velocity = 127;
    manager.Trigger(params);
    float l[48]{}, r[48]{};
    manager.Render(l, r, 48);
    uint32_t frame = 0;
    ASSERT_TRUE(FindVoicePlayhead(manager, pcm.data(), frame));
    EXPECT_GE(frame, 30u);
    EXPECT_LT(frame, 50u);
    params.start_frame = 5;
    params.loop = false;
    params.note = 61;
    manager.Trigger(params);
    ASSERT_TRUE(FindVoicePlayhead(manager, pcm.data(), frame));
    EXPECT_EQ(frame, 5u);
    params.sample = other.data();
    params.secondary.sample = pcm.data();
    params.secondary.sample_frames = 256;
    params.secondary.start_frame = 75;
    params.note = 62;
    manager.Trigger(params);
    ASSERT_TRUE(FindVoicePlayhead(manager, pcm.data(), frame));
    EXPECT_EQ(frame, 75u);
    EXPECT_FALSE(FindVoicePlayhead(manager, nullptr, frame));
    manager.Release(62);
    manager.Render(l, r, 1);
    ASSERT_TRUE(FindVoicePlayhead(manager, pcm.data(), frame));
    EXPECT_EQ(frame, 6u);
    manager.Release(61);
    manager.Render(l, r, 1);
    ASSERT_TRUE(FindVoicePlayhead(manager, pcm.data(), frame));
    EXPECT_EQ(frame, 30u);
    manager.StopTrack(0);
    EXPECT_FALSE(FindVoicePlayhead(manager, pcm.data(), frame));
}
