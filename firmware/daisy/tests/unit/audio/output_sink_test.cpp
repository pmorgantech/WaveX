// Host tests for the output sink stage (src/audio/output_sink.hpp).
//
// Every buffer here carries DISTINCT per-sample, per-voice, per-channel
// values (base + voice*100 + sample, negated on the right channel) - a
// constant-filled block cannot distinguish "summed voice v sample i" from
// "summed the wrong voice or the wrong sample", which is exactly the class
// of indexing bug a mixing stage can have.

#include "audio/output_sink.hpp"

#include <gtest/gtest.h>

#include <cstddef>

using WaveX::AudioEngine::StereoMixSink;
using WaveX::AudioEngine::TdmVoiceSink;
using WaveX::AudioEngine::VoiceBuffer;

namespace {

// voice v, sample i => left = v*100 + i + 1, right = -(v*100 + i + 1).
// Every (voice, sample, channel) triple is unique, so any cross-wiring
// changes the expected sums.
float LeftVal(size_t v, size_t i) {
    return static_cast<float>(v * 100 + i + 1);
}
float RightVal(size_t v, size_t i) {
    return -LeftVal(v, i);
}

}  // namespace

TEST(StereoMixSinkTest, SumsPerSamplePerVoicePerChannel) {
    constexpr size_t kVoices = 3;
    constexpr size_t kBlockSize = 4;
    float left[kVoices][kBlockSize];
    float right[kVoices][kBlockSize];
    VoiceBuffer voices[kVoices];
    for (size_t v = 0; v < kVoices; ++v) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            left[v][i] = LeftVal(v, i);
            right[v][i] = RightVal(v, i);
        }
        voices[v] = {left[v], right[v]};
    }

    float out_l[kBlockSize] = {0};
    float out_r[kBlockSize] = {0};
    StereoMixSink sink;
    sink.Process(voices, kVoices, kBlockSize, out_l, out_r);

    for (size_t i = 0; i < kBlockSize; ++i) {
        float expect_l = 0.0f, expect_r = 0.0f;
        for (size_t v = 0; v < kVoices; ++v) {
            expect_l += LeftVal(v, i);
            expect_r += RightVal(v, i);
        }
        EXPECT_FLOAT_EQ(out_l[i], expect_l) << "sample " << i;
        EXPECT_FLOAT_EQ(out_r[i], expect_r) << "sample " << i;
    }
}

TEST(StereoMixSinkTest, ZeroVoicesClearsStaleOutput) {
    constexpr size_t kBlockSize = 4;
    float out_l[kBlockSize] = {1.0f, 2.0f, 3.0f, 4.0f};
    float out_r[kBlockSize] = {-1.0f, -2.0f, -3.0f, -4.0f};

    StereoMixSink sink;
    sink.Process(nullptr, 0, kBlockSize, out_l, out_r);

    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_FLOAT_EQ(out_l[i], 0.0f);
        EXPECT_FLOAT_EQ(out_r[i], 0.0f);
    }
}

TEST(StereoMixSinkTest, ZeroBlockSizeTouchesNothing) {
    float voice[2] = {9.0f, 9.0f};
    VoiceBuffer voices[1] = {{voice, voice}};
    // Sentinels must survive a zero-length block untouched.
    float out_l[2] = {123.0f, 456.0f};
    float out_r[2] = {789.0f, -12.0f};

    StereoMixSink sink;
    sink.Process(voices, 1, 0, out_l, out_r);

    EXPECT_FLOAT_EQ(out_l[0], 123.0f);
    EXPECT_FLOAT_EQ(out_l[1], 456.0f);
    EXPECT_FLOAT_EQ(out_r[0], 789.0f);
    EXPECT_FLOAT_EQ(out_r[1], -12.0f);
}

TEST(StereoMixSinkTest, RespectsIndependentLeftRightPerVoice) {
    constexpr size_t kBlockSize = 2;
    float left[kBlockSize] = {0.5f, 0.25f};
    float right[kBlockSize] = {-0.5f, -0.125f};
    VoiceBuffer voices[1] = {{left, right}};

    float out_l[kBlockSize] = {0};
    float out_r[kBlockSize] = {0};
    StereoMixSink sink;
    sink.Process(voices, 1, kBlockSize, out_l, out_r);

    EXPECT_FLOAT_EQ(out_l[0], 0.5f);
    EXPECT_FLOAT_EQ(out_l[1], 0.25f);
    EXPECT_FLOAT_EQ(out_r[0], -0.5f);
    EXPECT_FLOAT_EQ(out_r[1], -0.125f);
}

// A mono voice played dual-mono passes the SAME pointer for both channels
// (documented in VoiceBuffer) - both outputs must receive it.
TEST(StereoMixSinkTest, MonoVoiceSharedPointerFeedsBothChannels) {
    float mono[3] = {0.1f, 0.2f, 0.3f};
    VoiceBuffer voices[1] = {{mono, mono}};

    float out_l[3] = {0}, out_r[3] = {0};
    StereoMixSink sink;
    sink.Process(voices, 1, 3, out_l, out_r);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(out_l[i], mono[i]);
        EXPECT_FLOAT_EQ(out_r[i], mono[i]);
    }
}

TEST(TdmVoiceSinkTest, EachVoiceLandsInItsOwnSlotSampleForSample) {
    constexpr size_t kBlockSize = 3;
    constexpr size_t kVoices = TdmVoiceSink::kNumSlots;
    float left[kVoices][kBlockSize];
    float right[kVoices][kBlockSize];
    VoiceBuffer voices[kVoices];
    for (size_t v = 0; v < kVoices; ++v) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            left[v][i] = LeftVal(v, i);
            right[v][i] = RightVal(v, i);
        }
        voices[v] = {left[v], right[v]};
    }

    float slot_storage[TdmVoiceSink::kNumSlots][kBlockSize] = {};
    float* slots[TdmVoiceSink::kNumSlots];
    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s)
        slots[s] = slot_storage[s];

    TdmVoiceSink sink;
    sink.Process(voices, kVoices, kBlockSize, slots);

    // The stub copies each voice's LEFT buffer only (documented TODO for the
    // Phase 3 SAI2 bring-up). left != right above pins that this is really
    // the left data; if the implementation grows real L/R routing, this
    // expectation must change deliberately.
    for (size_t v = 0; v < kVoices; ++v) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            EXPECT_FLOAT_EQ(slot_storage[v][i], LeftVal(v, i)) << "slot " << v << " sample " << i;
        }
    }
}

TEST(TdmVoiceSinkTest, SlotsBeyondVoiceCountAreLeftUntouched) {
    constexpr size_t kBlockSize = 2;
    float voice0[kBlockSize] = {0.1f, 0.2f};
    VoiceBuffer voices[1] = {{voice0, voice0}};

    float slot_storage[TdmVoiceSink::kNumSlots][kBlockSize];
    float* slots[TdmVoiceSink::kNumSlots];
    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s) {
        for (size_t i = 0; i < kBlockSize; ++i)
            slot_storage[s][i] = 42.0f;  // sentinel
        slots[s] = slot_storage[s];
    }

    TdmVoiceSink sink;
    sink.Process(voices, 1, kBlockSize, slots);

    EXPECT_FLOAT_EQ(slot_storage[0][0], 0.1f);
    EXPECT_FLOAT_EQ(slot_storage[0][1], 0.2f);
    for (size_t s = 1; s < TdmVoiceSink::kNumSlots; ++s) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            EXPECT_FLOAT_EQ(slot_storage[s][i], 42.0f) << "slot " << s << " was written";
        }
    }
}

// More voices than physical slots: the extras must be dropped, not written
// past the slot array.
TEST(TdmVoiceSinkTest, VoiceCountBeyondSlotCountIsClippedToSlots) {
    constexpr size_t kBlockSize = 2;
    constexpr size_t kVoices = TdmVoiceSink::kNumSlots + 4;
    float bufs[kVoices][kBlockSize];
    VoiceBuffer voices[kVoices];
    for (size_t v = 0; v < kVoices; ++v) {
        for (size_t i = 0; i < kBlockSize; ++i)
            bufs[v][i] = LeftVal(v, i);
        voices[v] = {bufs[v], bufs[v]};
    }

    float slot_storage[TdmVoiceSink::kNumSlots][kBlockSize] = {};
    float* slots[TdmVoiceSink::kNumSlots];
    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s)
        slots[s] = slot_storage[s];

    TdmVoiceSink sink;
    sink.Process(voices, kVoices, kBlockSize, slots);

    // Slots 0..7 carry voices 0..7; voices 8..11 have nowhere to go.
    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            EXPECT_FLOAT_EQ(slot_storage[s][i], LeftVal(s, i)) << "slot " << s;
        }
    }
}

// A null slot pointer (backend without that output wired) is skipped, and
// its voice's data must not spill into a neighbouring slot.
TEST(TdmVoiceSinkTest, NullSlotPointerIsSkippedSafely) {
    constexpr size_t kBlockSize = 2;
    float v0[kBlockSize] = {1.0f, 2.0f};
    float v1[kBlockSize] = {3.0f, 4.0f};
    VoiceBuffer voices[2] = {{v0, v0}, {v1, v1}};

    float slot1_storage[kBlockSize] = {0};
    float* slots[TdmVoiceSink::kNumSlots] = {nullptr};
    slots[1] = slot1_storage;

    TdmVoiceSink sink;
    sink.Process(voices, 2, kBlockSize, slots);

    EXPECT_FLOAT_EQ(slot1_storage[0], 3.0f);
    EXPECT_FLOAT_EQ(slot1_storage[1], 4.0f);
}

TEST(TdmVoiceSinkTest, ZeroBlockSizeWritesNothing) {
    float v0[2] = {1.0f, 2.0f};
    VoiceBuffer voices[1] = {{v0, v0}};

    float slot_storage[TdmVoiceSink::kNumSlots][2];
    float* slots[TdmVoiceSink::kNumSlots];
    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s) {
        slot_storage[s][0] = 7.0f;
        slot_storage[s][1] = 8.0f;
        slots[s] = slot_storage[s];
    }

    TdmVoiceSink sink;
    sink.Process(voices, 1, 0, slots);

    for (size_t s = 0; s < TdmVoiceSink::kNumSlots; ++s) {
        EXPECT_FLOAT_EQ(slot_storage[s][0], 7.0f);
        EXPECT_FLOAT_EQ(slot_storage[s][1], 8.0f);
    }
}
