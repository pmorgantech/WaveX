#include "audio/parameter_locks.hpp"

#include <gtest/gtest.h>

#include "sequencer/sequencer_transport.hpp"
#include <array>

namespace {
using namespace WaveX::AudioEngine;
using namespace WaveX::Sequencer;
using namespace WaveX::Protocol;

TEST(ParameterLocks, MapsControlsAndLeavesBaseAndOtherNotesAlone) {
    VoiceTriggerParams base;
    base.filter_cutoff_hz = 12000;
    base.pan = .2f;
    auto locked = base;
    const ParamLock locks[] = {{PARAM_FILTER_CUTOFF, 0},
                               {PARAM_PAN, 65535},
                               {PARAM_ENVELOPE_ATTACK, 65535},
                               {PARAM_PITCH, 65535}};
    ApplyParamLocks(locked, locks, 4);
    EXPECT_FLOAT_EQ(locked.filter_cutoff_hz, 20);
    EXPECT_FLOAT_EQ(locked.pan, 1);
    EXPECT_FLOAT_EQ(locked.attack_s, 2.001f);
    EXPECT_FLOAT_EQ(locked.locked_pitch_scale, 4);
    EXPECT_FLOAT_EQ(base.filter_cutoff_hz, 12000);
    EXPECT_FLOAT_EQ(base.pan, .2f);
    EXPECT_EQ(base.param_lock_mask, 0);
    auto next = base;
    ApplyParamLocks(next, nullptr, 255);
    EXPECT_FLOAT_EQ(next.filter_cutoff_hz, base.filter_cutoff_hz);
}

TEST(ParameterLocks, StartOffsetsStayInTheResolvedRegionAndKeepLoopLength) {
    VoiceTriggerParams p;
    p.sample_frames = 40000000;
    p.start_frame = 100;
    p.end_frame = 30000000;
    p.loop = true;
    p.loop_start = 200;
    p.loop_end = 20000000;
    const ParamLock locks[] = {{PARAM_SAMPLE_START, 65535}, {PARAM_LOOP_START, 65535}};
    ApplyParamLocks(p, locks, 2);
    EXPECT_EQ(p.start_frame, 29999998u);
    EXPECT_EQ(p.loop_start, 19999998u);
    EXPECT_EQ(p.end_frame, 30000000u);
    EXPECT_EQ(p.loop_end, 20000000u);
    p.sample_frames = p.end_frame = 1;
    p.start_frame = 0;
    p.loop = false;
    p.loop_start = 0;
    ApplyParamLocks(p, locks, 2);
    EXPECT_EQ(p.start_frame, 0u);
    EXPECT_EQ(p.loop_start, 0u);
}

TEST(ParameterLocks, UnsupportedIdsAndExcessLocksHaveNoEffect) {
    VoiceTriggerParams p;
    const ParamLock locks[] = {{PARAM_VOLUME, 0},
                               {0xFF, 65535},
                               {PARAM_LFO_RATE, 0},
                               {PARAM_MODULATION_MATRIX, 0},
                               {PARAM_PAN, 0}};
    ApplyParamLocks(p, locks, 255);
    EXPECT_EQ(p.param_lock_mask, 0);
    EXPECT_FLOAT_EQ(p.pan, .5f);
    EXPECT_FLOAT_EQ(p.gain_mul, 1);
}

TEST(ParameterLocks, LiveEditsPreserveOnlyLockedFieldsAndModulationComposes) {
    std::array<int16_t, 4096> sample{};
    VoiceManager voices;
    voices.Init(48000);
    VoiceTriggerParams base;
    base.sample = sample.data();
    base.sample_frames = sample.size();
    base.loop = true;
    auto locked = base;
    const ParamLock locks[] = {{PARAM_FILTER_CUTOFF, 0},
                               {PARAM_ENVELOPE_ATTACK, 65535},
                               {PARAM_PITCH, 65535},
                               {PARAM_PAN, 65535}};
    ApplyParamLocks(locked, locks, 4);
    voices.Trigger(locked);
    voices.Trigger(base);
    VoiceLiveParams edit;
    edit.filter_cutoff_hz = 5000;
    edit.filter_resonance = .7f;
    edit.attack_s = .1f;
    edit.decay_s = .3f;
    edit.sustain_level = .4f;
    edit.pan = .25f;
    edit.pitch_semitones = -12;
    voices.ApplyLiveParams(edit);
    const auto& a = voices.GetVoice(0);
    const auto& b = voices.GetVoice(1);
    EXPECT_FLOAT_EQ(a.base_cutoff_hz, 20);
    EXPECT_FLOAT_EQ(a.base_resonance, .7f);
    EXPECT_FLOAT_EQ(a.amp_params.attack, 2.001f);
    EXPECT_FLOAT_EQ(a.amp_params.decay, .3f);
    EXPECT_FLOAT_EQ(a.amp_params.sustain, .4f);
    EXPECT_FLOAT_EQ(a.pan, 1);
    EXPECT_FLOAT_EQ(a.increment, 4);
    EXPECT_FLOAT_EQ(b.increment, .5f);
    EXPECT_FLOAT_EQ(b.base_cutoff_hz, 5000);
    EXPECT_FLOAT_EQ(b.amp_params.attack, .1f);
    std::array<float, 48> left{}, right{};
    voices.Render(left.data(), right.data(), left.size());
    EXPECT_FLOAT_EQ(voices.GetVoice(0).increment, 4);
    ModSlot slots[kMaxModSlots]{};
    slots[0] = {SRC_MACRO_1, DEST_PITCH, 16384, CURVE_LINEAR, 0};
    const ModSlotResolver resolver{
        slots, [](const void* ctx, uint8_t) { return static_cast<const ModSlot*>(ctx); }};
    ModSources sources;
    sources.macro[0] = 1;
    voices.TickModulation(resolver, sources, 48);
    voices.Render(left.data(), right.data(), left.size());
    EXPECT_GT(voices.GetVoice(0).increment, 4);
    sources.macro[0] = 0;
    voices.TickModulation(resolver, sources, 48);
    voices.Render(left.data(), right.data(), left.size());
    EXPECT_FLOAT_EQ(voices.GetVoice(0).increment, 4);
    voices.StopAll();
    voices.Trigger(base);
    EXPECT_EQ(voices.GetVoice(0).param_lock_mask, 0);
    EXPECT_FLOAT_EQ(voices.GetVoice(0).increment, .5f);
}

TEST(ParameterLocks, ResonanceSustainReleaseAndGainMappings) {
    VoiceTriggerParams p;
    p.gain_mul = .25f;
    ParamLock locks[] = {{PARAM_FILTER_RESONANCE, 65535},
                         {PARAM_ENVELOPE_SUSTAIN, 0},
                         {PARAM_ENVELOPE_RELEASE, 65535},
                         {PARAM_GAIN, 32768}};
    ApplyParamLocks(p, locks, 4);
    EXPECT_FLOAT_EQ(p.filter_resonance, 1);
    EXPECT_FLOAT_EQ(p.sustain_level, 0);
    EXPECT_FLOAT_EQ(p.release_s, 2.001f);
    EXPECT_FLOAT_EQ(p.gain_mul, .25f);
}

TEST(ParameterLockEditing, SlotReplacementIsAtomicUniqueAndBounded) {
    SequencerTransport transport;
    transport.Init(48000, 48);
    transport.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK_SLOT, 15, 63, PARAM_PAN, 12345, 3});
    transport.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK_SLOT, 15, 63, PARAM_PAN, 222, 2});
    transport.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK_SLOT, 15, 63, PARAM_VOLUME, 0, 3});
    SeqPatternSyncMessage page;
    transport.BuildPatternPage({1, 15, 48, 0}, page);
    EXPECT_EQ(page.steps[15].locks[3].parameter, PARAM_PAN);
    EXPECT_EQ(page.steps[15].locks[3].value, 12345);
    EXPECT_EQ(page.steps[15].locks[2].parameter, 0);
    transport.ApplyPatternOp({SEQ_OP_CLEAR_PARAM_LOCK, 15, 63, PARAM_PAN, 0, 0});
    transport.BuildPatternPage({2, 15, 48, 0}, page);
    EXPECT_EQ(page.steps[15].locks[3].parameter, 0);
}
}  // namespace
