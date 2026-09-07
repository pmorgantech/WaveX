#include "audio/callback_stop_fence.hpp"

#include <gtest/gtest.h>

#include "memory.h"

#include "audio/voice_manager.hpp"
#include <array>
#include <cstring>

namespace {
using namespace WaveX::AudioEngine;

TEST(CallbackStopFenceTest, NoCallbackMeansNoPermissionToReleaseSampleMemory) {
    CallbackStopFence fence;
    fence.Init();
    const uint32_t generation = fence.RequestStop(1);
    for (int poll = 0; poll < 1000; ++poll) {
        EXPECT_FALSE(fence.Complete(generation));
    }
}

TEST(CallbackStopFenceTest, CompletionFollowsVoiceStopAndAllowsSafeArenaReuse) {
    alignas(32) static std::array<uint8_t, 256 * 1024> arena;
    SampleMemMgr memory;
    ASSERT_TRUE(memory.init(arena.data(), arena.size(), 64 * 1024));
    wxsamp_t sample{};
    ASSERT_TRUE(memory.alloc(128, &sample));
    void* pointer = nullptr;
    ASSERT_TRUE(memory.ptr(sample, &pointer));
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams trigger;
    trigger.sample = static_cast<int16_t*>(pointer);
    trigger.sample_frames = 64;
    trigger.loop = true;
    trigger.track = 3;
    voices.Trigger(trigger);
    ASSERT_EQ(voices.ActiveVoiceCount(), 1);

    CallbackStopFence fence;
    fence.Init();
    const uint32_t generation = fence.RequestStop(1u << 3);
    ASSERT_TRUE(fence.ConsumeAndStop([&](uint16_t tracks) {
        EXPECT_EQ(tracks, 1u << 3);
        EXPECT_FALSE(fence.Complete(generation));
        voices.StopTrack(3);
    }));
    ASSERT_TRUE(fence.Complete(generation));
    EXPECT_EQ(voices.ActiveVoiceCount(), 0);
    memory.release(&sample);
    ASSERT_TRUE(memory.alloc(128, &sample));
    ASSERT_TRUE(memory.ptr(sample, &pointer));
    std::memset(pointer, 0x7F, sample.len);
    std::array<float, 48> left{}, right{};
    voices.Render(left.data(), right.data(), left.size());
    for (float value: left) {
        EXPECT_FLOAT_EQ(value, 0.0f);
    }
}

TEST(CallbackStopFenceTest, OlderAcknowledgementCannotCompleteANewerStop) {
    CallbackStopFence fence;
    fence.Init();
    const uint32_t first = fence.RequestStop(1);
    ASSERT_TRUE(fence.ConsumeAndStop([](uint16_t) {}));
    ASSERT_TRUE(fence.Complete(first));
    const uint32_t second = fence.RequestStop(2);
    EXPECT_FALSE(fence.Complete(second));
    ASSERT_TRUE(fence.ConsumeAndStop([](uint16_t tracks) { EXPECT_EQ(tracks, 2); }));
    EXPECT_TRUE(fence.Complete(second));
}

TEST(CallbackStopFenceTest, CoalescingKeepsEveryUnacknowledgedTrack) {
    CallbackStopFence fence;
    fence.Init();
    const uint32_t first = fence.RequestStop(1);
    const uint32_t second = fence.RequestStop(8);
    ASSERT_TRUE(fence.ConsumeAndStop([](uint16_t tracks) { EXPECT_EQ(tracks, 9); }));
    EXPECT_TRUE(fence.Complete(first));
    EXPECT_TRUE(fence.Complete(second));
    EXPECT_FALSE(fence.ConsumeAndStop([](uint16_t) { ADD_FAILURE(); }));
}
}  // namespace
