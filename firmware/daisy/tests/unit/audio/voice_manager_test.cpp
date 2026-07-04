#include "audio/voice_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using WaveX::AudioEngine::kNumVoices;
using WaveX::AudioEngine::VoiceManager;
using WaveX::AudioEngine::VoiceState;
using WaveX::AudioEngine::VoiceTriggerParams;

namespace {

// A simple ramp sample, easy to reason about under linear interpolation.
std::vector<int16_t> MakeRampSample(size_t frames, int16_t start, int16_t step) {
    std::vector<int16_t> s(frames);
    for (size_t i = 0; i < frames; ++i) {
        s[i] = static_cast<int16_t>(start + static_cast<int16_t>(i) * step);
    }
    return s;
}

// Params for tests that want a clean, constant, full-amplitude signal with
// no envelope ramp/decay and no filtering, matching the pre-ADSR/pre-filter
// test semantics: instant attack, no decay (sustain=1.0), instant release,
// filter wide open.
VoiceTriggerParams FlatParams(
    const int16_t* sample, uint32_t frames, uint8_t note, uint8_t velocity, float pan) {
    VoiceTriggerParams p;
    p.sample = sample;
    p.sample_frames = frames;
    p.note = note;
    p.velocity = velocity;
    p.pan = pan;
    p.root_note = note;           // pitch ratio 1.0
    p.filter_cutoff_hz = 1.0e6f;  // safely at/above Nyquist at any sample rate -> exact bypass
    p.attack_s = 0.0f;
    p.decay_s = 0.0f;
    p.sustain_level = 1.0f;
    p.release_s = 0.0f;
    return p;
}

}  // namespace

TEST(VoiceManagerTest, NoVoicesActiveProducesSilence) {
    VoiceManager vm;
    vm.Init(48000);
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
    vm.Init(48000);
    auto sample = MakeRampSample(100, 1000, 0);  // constant value 1000
    vm.Trigger(FlatParams(sample.data(), sample.size(), 60, 127, 0.5f));

    EXPECT_EQ(vm.ActiveVoiceCount(), 1);

    float out_l[4] = {0};
    float out_r[4] = {0};
    vm.Render(out_l, out_r, 4);

    float expected = (1000.0f / 32768.0f) * 1.0f /*gain*/ * 0.5f /*pan split*/;
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(out_l[i], expected, 1e-4f) << "sample " << i;
        EXPECT_NEAR(out_r[i], expected, 1e-4f) << "sample " << i;
    }
}

TEST(VoiceManagerTest, VelocityScalesGain) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(10, 32767, 0);
    vm.Trigger(FlatParams(sample.data(), sample.size(), 60, /*velocity=*/64, 0.5f));

    float out_l[1] = {0};
    float out_r[1] = {0};
    vm.Render(out_l, out_r, 1);

    float expected_gain = 64.0f / 127.0f;
    float expected = 1.0f * expected_gain * 0.5f;
    EXPECT_NEAR(out_l[0], expected, 1e-3f);
}

TEST(VoiceManagerTest, PanFullyLeftAndFullyRight) {
    auto sample = MakeRampSample(10, 32767, 0);

    VoiceManager vm_left;
    vm_left.Init(48000);
    vm_left.Trigger(FlatParams(sample.data(), sample.size(), 60, 127, /*pan=*/0.0f));
    float l[1] = {0}, r[1] = {0};
    vm_left.Render(l, r, 1);
    EXPECT_NEAR(l[0], 1.0f, 1e-3f);
    EXPECT_NEAR(r[0], 0.0f, 1e-3f);

    VoiceManager vm_right;
    vm_right.Init(48000);
    vm_right.Trigger(FlatParams(sample.data(), sample.size(), 60, 127, /*pan=*/1.0f));
    l[0] = r[0] = 0;
    vm_right.Render(l, r, 1);
    EXPECT_NEAR(l[0], 0.0f, 1e-3f);
    EXPECT_NEAR(r[0], 1.0f, 1e-3f);
}

TEST(VoiceManagerTest, PanIsClamped) {
    auto sample = MakeRampSample(10, 32767, 0);

    VoiceManager vm;
    vm.Init(48000);
    auto p = FlatParams(sample.data(), sample.size(), 60, 127, /*pan=*/5.0f);
    vm.Trigger(p);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).pan, 1.0f);

    VoiceManager vm2;
    vm2.Init(48000);
    auto p2 = FlatParams(sample.data(), sample.size(), 60, 127, /*pan=*/-5.0f);
    vm2.Trigger(p2);
    EXPECT_FLOAT_EQ(vm2.GetVoice(0).pan, 0.0f);
}

TEST(VoiceManagerTest, EightVoicesGetDistinctSlots) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    for (uint8_t note = 0; note < kNumVoices; ++note) {
        vm.Trigger(FlatParams(sample.data(), sample.size(), note, 100, 0.5f));
    }

    EXPECT_EQ(vm.ActiveVoiceCount(), kNumVoices);
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
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    for (uint8_t note = 0; note < kNumVoices; ++note) {
        vm.Trigger(FlatParams(sample.data(), sample.size(), note, 100, 0.5f));
    }
    ASSERT_EQ(vm.ActiveVoiceCount(), kNumVoices);

    // None of the 8 are releasing, so stealing must fall back to the
    // oldest-triggered voice (note=0).
    vm.Trigger(FlatParams(sample.data(), sample.size(), /*note=*/99, 100, 0.5f));

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

TEST(VoiceManagerTest, StealingPrefersReleasingVoiceOverOlderSustainingOne) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    for (uint8_t note = 0; note < kNumVoices; ++note) {
        vm.Trigger(FlatParams(sample.data(), sample.size(), note, 100, 0.5f));
    }
    // Release note 5 (not the oldest) - its envelope enters Release stage
    // immediately since attack/decay are instant (FlatParams).
    vm.Release(5);

    vm.Trigger(FlatParams(sample.data(), sample.size(), /*note=*/99, 100, 0.5f));

    bool found_note_5 = false;
    bool found_note_0 = false;
    for (uint8_t i = 0; i < kNumVoices; ++i) {
        if (vm.GetVoice(i).note == 5)
            found_note_5 = true;
        if (vm.GetVoice(i).note == 0)
            found_note_0 = true;
    }
    EXPECT_FALSE(found_note_5) << "the releasing voice should be stolen preferentially";
    EXPECT_TRUE(found_note_0) << "the oldest (but still sustaining) voice should be left alone";
}

TEST(VoiceManagerTest, ReleaseStartsDecayNotImmediateStop) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    p.release_s = 0.01f;  // real release tail this time
    vm.Trigger(p);
    ASSERT_EQ(vm.ActiveVoiceCount(), 1);

    vm.Release(60);

    // Still allocated immediately after Release() - only the envelope's
    // Release stage has started, the voice hasn't been freed yet.
    EXPECT_EQ(vm.ActiveVoiceCount(), 1);
    EXPECT_TRUE(vm.GetVoice(0).envelope.IsReleasing());

    // Render through the full release tail (0.01s @ 48kHz = 480 samples).
    float out_l[512] = {0};
    float out_r[512] = {0};
    vm.Render(out_l, out_r, 512);

    EXPECT_EQ(vm.ActiveVoiceCount(), 0) << "voice should be freed once its release tail completes";
}

TEST(VoiceManagerTest, ReleaseUnknownNoteIsNoOp) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);
    vm.Trigger(FlatParams(sample.data(), sample.size(), 60, 100, 0.5f));

    vm.Release(99);  // no voice playing note 99

    EXPECT_EQ(vm.ActiveVoiceCount(), 1);
    EXPECT_FALSE(vm.GetVoice(0).envelope.IsReleasing());
}

TEST(VoiceManagerTest, VoiceSelfStopsAfterEndOfSampleAndRelease) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(4, 0, 0);  // 4 frames

    // Instant release so hitting end_frame frees the voice within this
    // Render() call rather than needing a long release tail rendered too.
    vm.Trigger(FlatParams(sample.data(), sample.size(), 60, 100, 0.5f));

    float out_l[16] = {0};
    float out_r[16] = {0};
    vm.Render(out_l, out_r, 16);  // block larger than the sample

    EXPECT_EQ(vm.ActiveVoiceCount(), 0) << "voice should have self-stopped, not looped";
}

TEST(VoiceManagerTest, TriggerRejectsNullOrTooShortSample) {
    VoiceManager vm;
    vm.Init(48000);
    int16_t one_frame[1] = {123};

    vm.Trigger(FlatParams(nullptr, 100, 60, 100, 0.5f));
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);

    vm.Trigger(FlatParams(one_frame, 1, 60, 100, 0.5f));  // < 2 frames, can't interpolate
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);
}

TEST(VoiceManagerTest, RenderSumsMultipleConcurrentVoices) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample_a = MakeRampSample(10, 16384, 0);  // constant 0.5 in float
    auto sample_b = MakeRampSample(10, 16384, 0);

    vm.Trigger(FlatParams(sample_a.data(), sample_a.size(), 60, 127, /*pan=*/1.0f));  // full right
    vm.Trigger(FlatParams(sample_b.data(), sample_b.size(), 61, 127, /*pan=*/0.0f));  // full left

    float out_l[1] = {0};
    float out_r[1] = {0};
    vm.Render(out_l, out_r, 1);

    // voice A contributes 0.5 to R only, voice B contributes 0.5 to L only.
    EXPECT_NEAR(out_l[0], 0.5f, 1e-3f);
    EXPECT_NEAR(out_r[0], 0.5f, 1e-3f);
}

// --- Phase 1 item 4: pitch, loop points, filter, ADSR ------------------

TEST(VoiceManagerTest, PitchRatioFromNoteRelativeToRootNote) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), /*note=*/72, 100, 0.5f);
    p.root_note = 60;  // one octave below note 72
    vm.Trigger(p);

    EXPECT_NEAR(vm.GetVoice(0).increment, 2.0f, 1e-4f) << "one octave up should double the rate";
}

TEST(VoiceManagerTest, PitchRatioOneOctaveDown) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), /*note=*/48, 100, 0.5f);
    p.root_note = 60;  // one octave below root
    vm.Trigger(p);

    EXPECT_NEAR(vm.GetVoice(0).increment, 0.5f, 1e-4f);
}

TEST(VoiceManagerTest, SameNoteAsRootNoteIsUnityRate) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    p.root_note = 60;
    vm.Trigger(p);

    EXPECT_NEAR(vm.GetVoice(0).increment, 1.0f, 1e-4f);
}

// Native-rate compensation (dma-timing-review-2026-07-03.md Finding 1): a
// 44.1kHz sample on a 48kHz engine must advance 44100/48000 = 0.91875
// source frames per output frame to play at its recorded pitch.
TEST(VoiceManagerTest, NativeRateCompensationFor44k1SampleOn48kEngine) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    p.root_note = 60;          // pitch ratio 1.0 from the note
    p.sample_rate_hz = 44100;  // native rate differs from engine
    vm.Trigger(p);

    EXPECT_NEAR(vm.GetVoice(0).increment, 44100.0f / 48000.0f, 1e-5f);
}

// sample_rate_hz == 0 (the default) means "same as engine": no compensation.
TEST(VoiceManagerTest, ZeroSampleRateMeansNoCompensation) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    p.root_note = 60;
    p.sample_rate_hz = 0;
    vm.Trigger(p);

    EXPECT_NEAR(vm.GetVoice(0).increment, 1.0f, 1e-5f);
}

// Rate compensation and note-based pitch compose multiplicatively: an
// octave up on a 44.1k sample = 2.0 x 0.91875.
TEST(VoiceManagerTest, RateCompensationComposesWithNotePitch) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(1000, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), /*note=*/72, 100, 0.5f);
    p.root_note = 60;  // one octave below note 72 -> x2.0
    p.sample_rate_hz = 44100;
    vm.Trigger(p);

    EXPECT_NEAR(vm.GetVoice(0).increment, 2.0f * (44100.0f / 48000.0f), 1e-4f);
}

TEST(VoiceManagerTest, LoopingVoiceWrapsInsteadOfStopping) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(4, 0, 0);  // 4 frames

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    p.loop = true;
    p.loop_start = 0;
    p.loop_end = 4;
    vm.Trigger(p);

    float out_l[100] = {0};
    float out_r[100] = {0};
    vm.Render(out_l, out_r, 100);  // far more than 4 frames

    // A looping voice must still be active after playing well past its
    // natural sample length - it wraps instead of releasing/stopping.
    EXPECT_EQ(vm.ActiveVoiceCount(), 1);
    EXPECT_EQ(vm.GetVoice(0).state, VoiceState::Playing);
}

TEST(VoiceManagerTest, StartFrameOffsetsInitialPlaybackPosition) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(10, 0, 100);  // sample[i] == i*100

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 127, 0.5f);
    p.start_frame = 5;
    vm.Trigger(p);

    EXPECT_FLOAT_EQ(vm.GetVoice(0).phase, 5.0f);

    float out_l[1] = {0};
    float out_r[1] = {0};
    vm.Render(out_l, out_r, 1);

    // First rendered sample should reflect sample[5], not sample[0].
    float expected = (500.0f / 32768.0f) * 0.5f;
    EXPECT_NEAR(out_l[0], expected, 1e-3f);
}

TEST(VoiceManagerTest, EndFrameTruncatesPlaybackRegion) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(100, 0, 1);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    p.end_frame = 10;  // much shorter than the full 100-frame sample
    vm.Trigger(p);

    float out_l[64] = {0};
    float out_r[64] = {0};
    vm.Render(out_l, out_r, 64);

    EXPECT_EQ(vm.ActiveVoiceCount(), 0)
        << "voice should have released at end_frame=10, not played the full 100 frames";
}

TEST(VoiceManagerTest, FilterAttenuatesHighFrequencyContent) {
    // A signal alternating +/-full-scale every sample is entirely
    // high-frequency (Nyquist); a lowpass filter should sharply attenuate
    // it, whereas a wide-open filter (FlatParams default) leaves it intact.
    constexpr size_t kFrames = 200;
    std::vector<int16_t> nyquist(kFrames);
    for (size_t i = 0; i < kFrames; ++i) {
        nyquist[i] = (i % 2 == 0) ? 32767 : -32768;
    }

    VoiceManager vm_open;
    vm_open.Init(48000);
    vm_open.Trigger(FlatParams(nyquist.data(), nyquist.size(), 60, 127, 0.5f));

    VoiceManager vm_filtered;
    vm_filtered.Init(48000);
    VoiceTriggerParams p = FlatParams(nyquist.data(), nyquist.size(), 60, 127, 0.5f);
    p.filter_cutoff_hz = 200.0f;  // well below Nyquist
    vm_filtered.Trigger(p);

    constexpr size_t kBlock = 64;
    float open_l[kBlock] = {0}, open_r[kBlock] = {0};
    float filt_l[kBlock] = {0}, filt_r[kBlock] = {0};
    vm_open.Render(open_l, open_r, kBlock);
    vm_filtered.Render(filt_l, filt_r, kBlock);

    // Compare peak-to-peak amplitude near the end of the block (past the
    // filter's initial transient) - the filtered signal must be
    // substantially smaller.
    float open_peak = 0.0f, filt_peak = 0.0f;
    for (size_t i = kBlock - 16; i < kBlock; ++i) {
        open_peak = std::max(open_peak, std::fabs(open_l[i]));
        filt_peak = std::max(filt_peak, std::fabs(filt_l[i]));
    }
    EXPECT_GT(open_peak, 0.1f);
    EXPECT_LT(filt_peak, open_peak * 0.5f)
        << "a 200Hz lowpass should substantially attenuate a Nyquist-rate signal";
}

TEST(VoiceManagerTest, EnvelopeRampsUpDuringAttack) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(48000, 32767, 0);  // long constant-value sample

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 127, 0.5f);
    p.attack_s = 0.01f;  // 480 samples @ 48kHz
    p.decay_s = 0.0f;
    p.sustain_level = 1.0f;
    p.release_s = 0.0f;
    vm.Trigger(p);

    float out_l[600] = {0};
    float out_r[600] = {0};
    vm.Render(out_l, out_r, 600);

    // Output should start near zero (attack just beginning) and be much
    // louder by the time attack has completed (past sample 480).
    EXPECT_LT(std::fabs(out_l[0]), std::fabs(out_l[599]));
    EXPECT_NEAR(out_l[599], 0.5f, 0.05f) << "should be near full amplitude once attack completes";
}

TEST(VoiceManagerTest, ReleasedVoiceEnvelopeDecaysTowardZero) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(48000, 32767, 0);

    VoiceTriggerParams p = FlatParams(sample.data(), sample.size(), 60, 127, 0.5f);
    p.release_s = 0.01f;  // 480 samples
    vm.Trigger(p);

    // Run to full sustain first.
    float scratch_l[64] = {0}, scratch_r[64] = {0};
    vm.Render(scratch_l, scratch_r, 64);

    vm.Release(60);

    float out_l[600] = {0};
    float out_r[600] = {0};
    vm.Render(out_l, out_r, 600);

    EXPECT_GT(std::fabs(out_l[0]), std::fabs(out_l[599]))
        << "amplitude should be decaying during the release tail";
}

TEST(VoiceManagerTest, StereoSourceAveragesChannelsToMono) {
    VoiceManager vm;
    vm.Init(48000);
    // Interleaved stereo, 100 frames: L = 2000, R = 1000 -> mono avg 1500.
    std::vector<int16_t> sample(200);
    for (size_t f = 0; f < 100; ++f) {
        sample[f * 2] = 2000;
        sample[f * 2 + 1] = 1000;
    }
    VoiceTriggerParams p = FlatParams(sample.data(), 100, 60, 127, 0.5f);
    p.channels = 2;
    vm.Trigger(p);

    float out_l[4] = {0}, out_r[4] = {0};
    vm.Render(out_l, out_r, 4);

    float expected = (1500.0f / 32768.0f) * 0.5f /*pan split*/;
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(out_l[i], expected, 1e-4f) << "sample " << i;
        EXPECT_NEAR(out_r[i], expected, 1e-4f) << "sample " << i;
    }
}

TEST(VoiceManagerTest, StereoSourceInterpolatesPerFrameNotPerValue) {
    VoiceManager vm;
    vm.Init(48000);
    // Stereo ramp on both channels: frame f holds value 100*f. Played one
    // octave up (increment 2.0), output sample i must read frame 2*i - if
    // the interleave stride were mishandled, values would come from the
    // wrong channel/frame and break the 200*i progression.
    std::vector<int16_t> sample(2 * 64);
    for (size_t f = 0; f < 64; ++f) {
        sample[f * 2] = static_cast<int16_t>(100 * f);
        sample[f * 2 + 1] = static_cast<int16_t>(100 * f);
    }
    VoiceTriggerParams p = FlatParams(sample.data(), 64, 72, 127, 0.0f);  // fully left
    p.root_note = 60;                                                     // +12 semitones = 2x
    p.channels = 2;
    vm.Trigger(p);

    float out_l[8] = {0}, out_r[8] = {0};
    vm.Render(out_l, out_r, 8);

    for (int i = 0; i < 8; ++i) {
        float expected = (200.0f * static_cast<float>(i)) / 32768.0f;
        EXPECT_NEAR(out_l[i], expected, 1e-3f) << "sample " << i;
    }
}

TEST(VoiceManagerTest, StopAllSilencesEveryVoiceImmediately) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(48000, 32767, 0);
    for (uint8_t n = 0; n < kNumVoices; ++n) {
        vm.Trigger(
            FlatParams(sample.data(), sample.size(), static_cast<uint8_t>(60 + n), 127, 0.5f));
    }
    ASSERT_EQ(vm.ActiveVoiceCount(), kNumVoices);

    vm.StopAll();

    EXPECT_EQ(vm.ActiveVoiceCount(), 0);
    float out_l[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    float out_r[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    vm.Render(out_l, out_r, 8);
    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(out_l[i], 0.0f);
        EXPECT_FLOAT_EQ(out_r[i], 0.0f);
    }
}
