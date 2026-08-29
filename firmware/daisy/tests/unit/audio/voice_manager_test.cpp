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

// HeldVoiceCount is the paraphonic envelope's gate (Stage A, item 5): only
// voices that are sounding AND not yet releasing count as held.
TEST(VoiceManagerTest, HeldVoiceCountExcludesReleasingVoices) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(48000, 0, 0);  // long enough not to auto-release

    auto p = FlatParams(sample.data(), sample.size(), 60, 127, 0.5f);
    p.release_s = 1.0f;  // long release so the voice stays active after Release()
    vm.Trigger(p);
    p.note = 64;
    vm.Trigger(p);
    EXPECT_EQ(vm.HeldVoiceCount(), 2);
    EXPECT_EQ(vm.ActiveVoiceCount(), 2);

    vm.Release(64);
    // The released voice is still sounding (release tail) but no longer held.
    EXPECT_EQ(vm.HeldVoiceCount(), 1);
    EXPECT_EQ(vm.ActiveVoiceCount(), 2);

    vm.Release(60);
    EXPECT_EQ(vm.HeldVoiceCount(), 0);
    EXPECT_EQ(vm.ActiveVoiceCount(), 2);  // both in release tails

    vm.StopAll();
    EXPECT_EQ(vm.HeldVoiceCount(), 0);
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);
}

// ---- Instrument-model extensions (instrument-model.md §3/§10) ----

// Helper: find the voice index currently playing `note` (first match), or -1.
static int FindVoiceForNote(const VoiceManager& vm, uint8_t note) {
    for (uint8_t i = 0; i < kNumVoices; ++i) {
        const auto& v = vm.GetVoice(i);
        if (v.state != VoiceState::Idle && v.note == note)
            return i;
    }
    return -1;
}

TEST(VoiceManagerTest, GainMulScalesVelocityGain) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(100, 1000, 0);
    auto p = FlatParams(sample.data(), sample.size(), 60, 127, 0.5f);
    p.gain_mul = 0.5f;  // half gain
    vm.Trigger(p);

    // velocity 127 => base gain 1.0, × gain_mul 0.5 => 0.5.
    int idx = FindVoiceForNote(vm, 60);
    ASSERT_GE(idx, 0);
    EXPECT_FLOAT_EQ(vm.GetVoice(idx).gain, 0.5f);
}

TEST(VoiceManagerTest, PitchRatioMulMultipliesIncrement) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(100, 0, 1);
    auto p = FlatParams(sample.data(), sample.size(), 60, 127, 0.5f);
    p.root_note = 60;          // base 12-TET ratio = 1.0
    p.pitch_ratio_mul = 2.0f;  // one octave up via the multiplier
    vm.Trigger(p);

    int idx = FindVoiceForNote(vm, 60);
    ASSERT_GE(idx, 0);
    EXPECT_FLOAT_EQ(vm.GetVoice(idx).increment, 2.0f);
}

TEST(VoiceManagerTest, SlotAndChokeGroupAreStored) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(100, 1000, 0);
    auto p = FlatParams(sample.data(), sample.size(), 42, 100, 0.5f);
    p.slot = 3;
    p.choke_group = 2;
    vm.Trigger(p);

    int idx = FindVoiceForNote(vm, 42);
    ASSERT_GE(idx, 0);
    EXPECT_EQ(vm.GetVoice(idx).slot, 3);
    EXPECT_EQ(vm.GetVoice(idx).choke_group, 2);
}

TEST(VoiceManagerTest, ChokeGroupCutsOffPreviousVoiceInSameGroup) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(48000, 1000, 0);  // long, won't auto-release

    // Open hat: group 1, long release so it would otherwise ring.
    auto open_hat = FlatParams(sample.data(), sample.size(), 46, 100, 0.5f);
    open_hat.choke_group = 1;
    open_hat.release_s = 2.0f;
    vm.Trigger(open_hat);
    int open_idx = FindVoiceForNote(vm, 46);
    ASSERT_GE(open_idx, 0);
    EXPECT_EQ(vm.HeldVoiceCount(), 1);

    // Closed hat: same group 1 -> chokes the open hat into a fast release.
    auto closed_hat = FlatParams(sample.data(), sample.size(), 42, 100, 0.5f);
    closed_hat.choke_group = 1;
    vm.Trigger(closed_hat);

    // The open hat is now releasing (choked), the closed hat is held.
    EXPECT_TRUE(vm.GetVoice(open_idx).envelope.IsReleasing());
    EXPECT_EQ(vm.HeldVoiceCount(), 1);  // only the closed hat is held
}

TEST(VoiceManagerTest, ChokeDoesNotAffectOtherGroups) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(48000, 1000, 0);

    auto g1 = FlatParams(sample.data(), sample.size(), 46, 100, 0.5f);
    g1.choke_group = 1;
    g1.release_s = 2.0f;
    vm.Trigger(g1);

    auto g2 = FlatParams(sample.data(), sample.size(), 50, 100, 0.5f);
    g2.choke_group = 2;  // different group
    vm.Trigger(g2);

    // Trigger another group-1 voice - only the first (group 1) is choked.
    auto g1b = FlatParams(sample.data(), sample.size(), 42, 100, 0.5f);
    g1b.choke_group = 1;
    vm.Trigger(g1b);

    int g2_idx = FindVoiceForNote(vm, 50);
    ASSERT_GE(g2_idx, 0);
    EXPECT_FALSE(vm.GetVoice(g2_idx).envelope.IsReleasing());  // group 2 untouched
}

TEST(VoiceManagerTest, StopSlotStopsOnlyMatchingSlot) {
    VoiceManager vm;
    vm.Init(48000);
    auto sample = MakeRampSample(100, 1000, 0);

    auto a = FlatParams(sample.data(), sample.size(), 60, 100, 0.5f);
    a.slot = 1;
    vm.Trigger(a);
    auto b = FlatParams(sample.data(), sample.size(), 62, 100, 0.5f);
    b.slot = 2;
    vm.Trigger(b);
    auto c = FlatParams(sample.data(), sample.size(), 64, 100, 0.5f);
    c.slot = 1;
    vm.Trigger(c);
    EXPECT_EQ(vm.ActiveVoiceCount(), 3);

    vm.StopSlot(1);  // stops the two slot-1 voices, leaves slot 2
    EXPECT_EQ(vm.ActiveVoiceCount(), 1);
    int b_idx = FindVoiceForNote(vm, 62);
    EXPECT_GE(b_idx, 0);  // slot-2 voice survives
}

// Region fades on the RAM path (roadmap 1.5.6 item 3). The streaming audition
// and a note-triggered voice must agree about a sample's fades, or the editor
// would audition something a pad does not play - which is the same divergence
// SampleMetadata exists to close.
TEST(VoiceManagerFadeTest, RegionFadeRampsTheHeadOfTheSample) {
    // Constant full-scale source, so anything below full scale in the output
    // is the fade and nothing else.
    std::vector<int16_t> sample(4096, 16000);

    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);

    WaveX::AudioEngine::VoiceTriggerParams p;
    p.sample = sample.data();
    p.sample_frames = static_cast<uint32_t>(sample.size());
    p.sample_rate_hz = 48000;
    p.velocity = 127;
    p.pan = 0.5f;
    p.attack_s = 0.0f;  // isolate the fade from the ADSR
    p.decay_s = 0.0f;
    p.sustain_level = 1.0f;
    p.fade_in_ms = 10;  // 480 frames
    p.fade_out_ms = 0;
    vm.Trigger(p);

    std::vector<float> l(256), r(256);
    vm.Render(l.data(), r.data(), l.size());

    EXPECT_NEAR(l[0], 0.0f, 1e-4f) << "the first frame must start from silence";
    EXPECT_LT(l[10], l[100]) << "the fade must be rising";
    EXPECT_LT(l[100], l[250]);
}

TEST(VoiceManagerFadeTest, NoFadeRequestedMeansUnityNotSilence) {
    std::vector<int16_t> sample(4096, 16000);

    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);

    WaveX::AudioEngine::VoiceTriggerParams p;
    p.sample = sample.data();
    p.sample_frames = static_cast<uint32_t>(sample.size());
    p.sample_rate_hz = 48000;
    p.velocity = 127;
    p.pan = 0.5f;
    p.attack_s = 0.0f;
    p.decay_s = 0.0f;
    p.sustain_level = 1.0f;
    p.fade_in_ms = 0;
    p.fade_out_ms = 0;
    vm.Trigger(p);

    std::vector<float> l(16), r(16);
    vm.Render(l.data(), r.data(), l.size());
    EXPECT_GT(l[0], 0.1f) << "an absent fade must not mute the head";
}

// --- Live parameter edits (digital-voice-audition.md stage 1) --------------
//
// Before ApplyLiveParams existed, the per-voice filter and envelope were
// written once at Trigger() time, so nothing could change a voice that was
// already sounding. These pin the behaviour that makes "sweep the filter
// while a note plays" work, and the one case where it must NOT reach in.

namespace {

// A steady full-scale square at Nyquist - entirely high-frequency content, so
// a lowpass acting on it shows up as an amplitude drop.
std::vector<int16_t> NyquistTone(size_t frames) {
    std::vector<int16_t> s(frames);
    for (size_t i = 0; i < frames; ++i)
        s[i] = (i % 2 == 0) ? 32767 : -32768;
    return s;
}

float PeakOf(const std::vector<float>& v, size_t from) {
    float peak = 0.0f;
    for (size_t i = from; i < v.size(); ++i)
        peak = std::max(peak, std::fabs(v[i]));
    return peak;
}

}  // namespace

TEST(VoiceManagerLiveParamsTest, CutoffChangeReachesASoundingVoice) {
    const std::vector<int16_t> tone = NyquistTone(4096);

    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);
    vm.Trigger(FlatParams(tone.data(), static_cast<uint32_t>(tone.size()), 60, 127, 0.5f));

    // Wide open (FlatParams bypasses the filter): the tone passes.
    std::vector<float> l(64), r(64);
    vm.Render(l.data(), r.data(), l.size());
    const float open_peak = PeakOf(l, 48);
    ASSERT_GT(open_peak, 0.1f);

    // Close the filter on the ALREADY SOUNDING voice - no retrigger.
    WaveX::AudioEngine::VoiceLiveParams live;
    live.filter_cutoff_hz = 200.0f;
    vm.ApplyLiveParams(live);

    std::vector<float> l2(256), r2(256);
    vm.Render(l2.data(), r2.data(), l2.size());
    EXPECT_LT(PeakOf(l2, 192), open_peak * 0.5f)
        << "closing the cutoff must attenuate a voice that is already playing";
}

TEST(VoiceManagerLiveParamsTest, IdleVoicesAreUntouched) {
    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);
    WaveX::AudioEngine::VoiceLiveParams live;
    live.filter_cutoff_hz = 200.0f;
    vm.ApplyLiveParams(live);  // must not fault or wake anything
    EXPECT_EQ(vm.ActiveVoiceCount(), 0);
}

TEST(VoiceManagerLiveParamsTest, SustainChangeReachesASoundingVoice) {
    std::vector<int16_t> dc(8192, 20000);

    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);
    WaveX::AudioEngine::VoiceTriggerParams p =
        FlatParams(dc.data(), static_cast<uint32_t>(dc.size()), 60, 127, 0.5f);
    p.sustain_level = 1.0f;
    vm.Trigger(p);

    std::vector<float> l(64), r(64);
    vm.Render(l.data(), r.data(), l.size());
    const float full = PeakOf(l, 32);
    ASSERT_GT(full, 0.1f);

    WaveX::AudioEngine::VoiceLiveParams live;
    live.attack_s = 0.0f;
    live.decay_s = 0.0f;
    live.sustain_level = 0.25f;  // quarter level
    live.release_s = 0.0f;
    vm.ApplyLiveParams(live);

    std::vector<float> l2(256), r2(256);
    vm.Render(l2.data(), r2.data(), l2.size());
    EXPECT_LT(PeakOf(l2, 192), full * 0.5f)
        << "lowering sustain must be audible on a voice that is already playing";
}

// The important negative case. Choke() forces a ~5 ms release onto a voice and
// then releases it; if a live ADSR edit rewrote that release time mid-choke,
// the choked voice would get its full-length release back and an open hat
// would not cut off when the closed hat fired.
TEST(VoiceManagerLiveParamsTest, DoesNotResurrectAChokedVoicesReleaseTime) {
    std::vector<int16_t> dc(48000, 20000);

    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);
    WaveX::AudioEngine::VoiceTriggerParams p =
        FlatParams(dc.data(), static_cast<uint32_t>(dc.size()), 60, 127, 0.5f);
    p.choke_group = 1;
    p.release_s = 5.0f;  // a very long natural release
    vm.Trigger(p);

    // Render first so the envelope actually reaches its sustain level. Choking
    // a voice whose level is still 0 frees it on the next sample whatever its
    // release time is, which would make this test pass for the wrong reason.
    std::vector<float> warm(64), warm_r(64);
    vm.Render(warm.data(), warm_r.data(), warm.size());
    ASSERT_GT(PeakOf(warm, 32), 0.1f) << "voice should be sounding before the choke";

    vm.Choke(1, 0.005f);  // 5 ms forced release

    // A live edit arrives mid-choke asking for a long release again.
    WaveX::AudioEngine::VoiceLiveParams live;
    live.release_s = 5.0f;
    vm.ApplyLiveParams(live);

    // 5 ms at 48 kHz is 240 frames; render well past that.
    std::vector<float> l(2048), r(2048);
    vm.Render(l.data(), r.data(), l.size());
    EXPECT_EQ(vm.ActiveVoiceCount(), 0)
        << "a live ADSR edit must not extend a choked voice's forced release";
}

TEST(VoiceManagerLiveParamsTest, FilterStillTracksThroughTheReleaseTail) {
    // Filter edits, unlike envelope edits, SHOULD reach a releasing voice - a
    // sweep that stopped at note-off would sound like the filter jammed.
    const std::vector<int16_t> tone = NyquistTone(48000);

    WaveX::AudioEngine::VoiceManager vm;
    vm.Init(48000);
    WaveX::AudioEngine::VoiceTriggerParams p =
        FlatParams(tone.data(), static_cast<uint32_t>(tone.size()), 60, 127, 0.5f);
    p.release_s = 2.0f;  // long tail so the voice stays alive while we look
    vm.Trigger(p);

    std::vector<float> warm(64), warm_r(64);
    vm.Render(warm.data(), warm_r.data(), warm.size());
    const float open_peak = PeakOf(warm, 48);
    ASSERT_GT(open_peak, 0.1f);

    vm.Release(60);
    ASSERT_EQ(vm.ActiveVoiceCount(), 1) << "voice should still be in its release tail";

    WaveX::AudioEngine::VoiceLiveParams live;
    live.filter_cutoff_hz = 200.0f;
    vm.ApplyLiveParams(live);

    std::vector<float> l(256), r(256);
    vm.Render(l.data(), r.data(), l.size());
    EXPECT_LT(PeakOf(l, 192), open_peak * 0.5f)
        << "a filter sweep must stay audible through the release tail";
}
