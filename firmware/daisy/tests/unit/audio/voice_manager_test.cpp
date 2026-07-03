#include "audio/voice_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using WaveX::AudioEngine::kNumVoices;
using WaveX::AudioEngine::VoiceManager;
using WaveX::AudioEngine::VoiceState;

namespace {

// A simple ramp sample, easy to reason about under linear interpolation.
std::vector<int16_t> MakeRampSample(size_t frames, int16_t start, int16_t step) {
    std::vector<int16_t> s(frames);
    for (size_t i = 0; i < frames; ++i) {
        s[i] = static_cast<int16_t>(start + static_cast<int16_t>(i) * step);
    }
    return s;
}

}  // namespace

TEST(VoiceManagerTest, NoVoicesActiveProducesSilence) {
    VoiceManager vm;
    float out_l[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float out_r[8] = {1, 1, 1, 1, 1, 1, 1, 1};

    vm.Render(out_l, out_r, 8);

    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(out_l[i], 0.0f);
        EXPECT_FLOAT_EQ(out_r[i], 0.0f);
    }
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);
}

TEST(VoiceManagerTest, TriggerAllocatesAndRenders) {
    VoiceManager vm;
    auto sample = MakeRampSample(100, 1000, 0);  // constant value 1000
    vm.Trigger(sample.data(), sample.size(), /*note=*/60, /*velocity=*/127, /*pan=*/0.5f);

    EXPECT_EQ(vm.ActiveVoiceCount(), 1);

    float out_l[4] = {0};
    float out_r[4] = {0};
    vm.Render(out_l, out_r, 4);

    float expected = (1000.0f / 32768.0f) * 1.0f /*gain*/ * 0.5f /*pan split*/;
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(out_l[i], expected, 1e-5f) << "sample " << i;
        EXPECT_NEAR(out_r[i], expected, 1e-5f) << "sample " << i;
    }
}

TEST(VoiceManagerTest, VelocityScalesGain) {
    VoiceManager vm;
    auto sample = MakeRampSample(10, 32767, 0);
    vm.Trigger(sample.data(), sample.size(), 60, /*velocity=*/64, 0.5f);

    float out_l[1] = {0};
    float out_r[1] = {0};
    vm.Render(out_l, out_r, 1);

    float expected_gain = 64.0f / 127.0f;
    float expected = 1.0f * expected_gain * 0.5f;
    EXPECT_NEAR(out_l[0], expected, 1e-4f);
}

TEST(VoiceManagerTest, PanFullyLeftAndFullyRight) {
    VoiceManager vm_left;
    auto sample = MakeRampSample(10, 32767, 0);
    vm_left.Trigger(sample.data(), sample.size(), 60, 127, /*pan=*/0.0f);
    float l[1] = {0}, r[1] = {0};
    vm_left.Render(l, r, 1);
    EXPECT_NEAR(l[0], 1.0f, 1e-4f);
    EXPECT_NEAR(r[0], 0.0f, 1e-4f);

    VoiceManager vm_right;
    vm_right.Trigger(sample.data(), sample.size(), 60, 127, /*pan=*/1.0f);
    l[0] = r[0] = 0;
    vm_right.Render(l, r, 1);
    EXPECT_NEAR(l[0], 0.0f, 1e-4f);
    EXPECT_NEAR(r[0], 1.0f, 1e-4f);
}

TEST(VoiceManagerTest, PanIsClamped) {
    VoiceManager vm;
    auto sample = MakeRampSample(10, 32767, 0);
    vm.Trigger(sample.data(), sample.size(), 60, 127, /*pan=*/5.0f);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).pan, 1.0f);

    VoiceManager vm2;
    vm2.Trigger(sample.data(), sample.size(), 60, 127, /*pan=*/-5.0f);
    EXPECT_FLOAT_EQ(vm2.GetVoice(0).pan, 0.0f);
}

TEST(VoiceManagerTest, EightVoicesGetDistinctSlots) {
    VoiceManager vm;
    auto sample = MakeRampSample(1000, 0, 1);

    for (uint8_t note = 0; note < kNumVoices; ++note) {
        vm.Trigger(sample.data(), sample.size(), note, 100, 0.5f);
    }

    EXPECT_EQ(vm.ActiveVoiceCount(), kNumVoices);
    // Every voice should have a distinct note (proves distinct slots, not
    // one voice repeatedly reused).
    std::vector<uint8_t> notes;
    for (uint8_t i = 0; i < kNumVoices; ++i) {
        notes.push_back(vm.GetVoice(i).note);
    }
    std::sort(notes.begin(), notes.end());
    for (uint8_t i = 0; i < kNumVoices; ++i) {
        EXPECT_EQ(notes[i], i);
    }
}

TEST(VoiceManagerTest, NinthTriggerStealsOldestVoice) {
    VoiceManager vm;
    auto sample = MakeRampSample(1000, 0, 1);

    for (uint8_t note = 0; note < kNumVoices; ++note) {
        vm.Trigger(sample.data(), sample.size(), note, 100, 0.5f);
    }
    ASSERT_EQ(vm.ActiveVoiceCount(), kNumVoices);

    // The 9th trigger must steal voice 0 (the oldest, note=0) rather than
    // silently being dropped or growing beyond 8 voices.
    vm.Trigger(sample.data(), sample.size(), /*note=*/99, 100, 0.5f);

    EXPECT_EQ(vm.ActiveVoiceCount(), kNumVoices);
    bool found_note_0 = false;
    bool found_note_99 = false;
    for (uint8_t i = 0; i < kNumVoices; ++i) {
        if (vm.GetVoice(i).note == 0)
            found_note_0 = true;
        if (vm.GetVoice(i).note == 99)
            found_note_99 = true;
    }
    EXPECT_FALSE(found_note_0) << "oldest voice should have been stolen";
    EXPECT_TRUE(found_note_99) << "9th trigger should have taken the stolen slot";
}

TEST(VoiceManagerTest, ReleaseStopsMatchingNote) {
    VoiceManager vm;
    auto sample = MakeRampSample(1000, 0, 1);
    vm.Trigger(sample.data(), sample.size(), 60, 100, 0.5f);
    vm.Trigger(sample.data(), sample.size(), 61, 100, 0.5f);
    ASSERT_EQ(vm.ActiveVoiceCount(), 2);

    vm.Release(60);

    EXPECT_EQ(vm.ActiveVoiceCount(), 1);
    EXPECT_EQ(vm.GetVoice(1).note, 61);
    EXPECT_EQ(vm.GetVoice(1).state, VoiceState::Playing);
}

TEST(VoiceManagerTest, ReleaseUnknownNoteIsNoOp) {
    VoiceManager vm;
    auto sample = MakeRampSample(1000, 0, 1);
    vm.Trigger(sample.data(), sample.size(), 60, 100, 0.5f);

    vm.Release(99);  // no voice playing note 99

    EXPECT_EQ(vm.ActiveVoiceCount(), 1);
}

TEST(VoiceManagerTest, VoiceSelfStopsAtSampleEnd) {
    VoiceManager vm;
    auto sample = MakeRampSample(4, 0, 0);  // 4 frames
    vm.Trigger(sample.data(), sample.size(), 60, 100, 0.5f);

    float out_l[16] = {0};
    float out_r[16] = {0};
    vm.Render(out_l, out_r, 16);  // block larger than the sample

    EXPECT_EQ(vm.ActiveVoiceCount(), 0) << "voice should have self-stopped, not looped";
}

TEST(VoiceManagerTest, TriggerRejectsNullOrTooShortSample) {
    VoiceManager vm;
    int16_t one_frame[1] = {123};

    vm.Trigger(nullptr, 100, 60, 100, 0.5f);
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);

    vm.Trigger(one_frame, 1, 60, 100, 0.5f);  // < 2 frames, can't interpolate
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);
}

TEST(VoiceManagerTest, RenderSumsMultipleConcurrentVoices) {
    VoiceManager vm;
    auto sample_a = MakeRampSample(10, 16384, 0);  // constant 0.5 in float
    auto sample_b = MakeRampSample(10, 16384, 0);

    vm.Trigger(sample_a.data(), sample_a.size(), 60, 127, /*pan=*/1.0f);  // full right
    vm.Trigger(sample_b.data(), sample_b.size(), 61, 127, /*pan=*/0.0f);  // full left

    float out_l[1] = {0};
    float out_r[1] = {0};
    vm.Render(out_l, out_r, 1);

    // voice A contributes 0.5 to R only, voice B contributes 0.5 to L only.
    EXPECT_NEAR(out_l[0], 0.5f, 1e-3f);
    EXPECT_NEAR(out_r[0], 0.5f, 1e-3f);
}
