#include "audio/output_sink.hpp"

#include <gtest/gtest.h>

using WaveX::AudioEngine::StereoMixSink;
using WaveX::AudioEngine::TdmVoiceSink;
using WaveX::AudioEngine::VoiceBuffer;

TEST(StereoMixSinkTest, SumsMultipleVoices) {
    constexpr size_t kBlockSize = 4;
    float voice0[kBlockSize] = {0.1f, 0.1f, 0.1f, 0.1f};
    float voice1[kBlockSize] = {0.2f, 0.2f, 0.2f, 0.2f};
    VoiceBuffer voices[2] = {{voice0, voice0}, {voice1, voice1}};

    float out_l[kBlockSize] = {0};
    float out_r[kBlockSize] = {0};

    StereoMixSink sink;
    sink.Process(voices, 2, kBlockSize, out_l, out_r);

    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(out_l[i], 0.3f) << "sample " << i;
        EXPECT_FLOAT_EQ(out_r[i], 0.3f) << "sample " << i;
    }
}

TEST(StereoMixSinkTest, ZeroVoicesProducesSilence) {
    constexpr size_t kBlockSize = 4;
    float out_l[kBlockSize] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out_r[kBlockSize] = {1.0f, 1.0f, 1.0f, 1.0f};

    StereoMixSink sink;
    sink.Process(nullptr, 0, kBlockSize, out_l, out_r);

    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(out_l[i], 0.0f);
        EXPECT_FLOAT_EQ(out_r[i], 0.0f);
    }
}

TEST(StereoMixSinkTest, RespectsIndependentLeftRightPerVoice) {
    constexpr size_t kBlockSize = 2;
    float left[kBlockSize] = {0.5f, 0.5f};
    float right[kBlockSize] = {-0.5f, -0.5f};
    VoiceBuffer voices[1] = {{left, right}};

    float out_l[kBlockSize] = {0};
    float out_r[kBlockSize] = {0};

    StereoMixSink sink;
    sink.Process(voices, 1, kBlockSize, out_l, out_r);

    EXPECT_FLOAT_EQ(out_l[0], 0.5f);
    EXPECT_FLOAT_EQ(out_r[0], -0.5f);
}

TEST(TdmVoiceSinkTest, CopiesVoiceIntoMatchingSlot) {
    constexpr size_t kBlockSize = 3;
    float voice0[kBlockSize] = {0.1f, 0.2f, 0.3f};
    VoiceBuffer voices[1] = {{voice0, voice0}};

    float slot_storage[TdmVoiceSink::kNumSlots][kBlockSize] = {};
    float* slots[TdmVoiceSink::kNumSlots];
    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s) {
        slots[s] = slot_storage[s];
    }

    TdmVoiceSink sink;
    sink.Process(voices, 1, kBlockSize, slots);

    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(slot_storage[0][i], voice0[i]) << "sample " << i;
    }
    // Untouched slots stay whatever the caller left them (zero here).
    for (size_t s = 1; s < TdmVoiceSink::kNumSlots; ++s) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            EXPECT_FLOAT_EQ(slot_storage[s][i], 0.0f);
        }
    }
}
