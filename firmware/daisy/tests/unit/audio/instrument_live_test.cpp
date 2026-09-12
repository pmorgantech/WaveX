#include <gtest/gtest.h>

#include "audio/instrument.hpp"
#include "audio/instrument_sound_undo.hpp"
#include "audio/parameter_locks.hpp"
#include "audio/sequencer_voice_map.hpp"
#include <array>
#include <cmath>
using namespace WaveX::AudioEngine;
namespace {
class InstrumentLiveTest : public ::testing::Test {
   protected:
    Instrument ins;
    VoiceManager vm;
    std::array<int16_t, 4096> pcm{};
    SampleRef ref;
    Zone zone;
    void SetUp() override {
        vm.Init(48000);
        pcm.fill(16384);
        ref.data = pcm.data();
        ref.frames = pcm.size();
        ref.sample_rate_hz = 48000;
        ref.loop_enabled = true;
        ins.origin = InstrumentOrigin::Built;
        ins.env[0] = {0, 0, 1, .1f};
        zone.in_use = true;
        zone.sample_id = 1;
    }
    VoiceTriggerParams TriggerParams(uint8_t note = 60, uint8_t osc = 0) {
        return PrepareZoneTrigger(ins, zone, ref, 0, note, 127, osc);
    }
    void Live(uint8_t track = 0) {
        VoiceLiveParams p;
        p.track = track;
        PrepareInstrumentLive(ins, p);
        vm.ApplyLiveParams(p);
    }
    float Render() {
        std::array<float, 48> left{}, right{};
        vm.Render(left.data(), right.data(), left.size());
        float peak = 0;
        for (float v: left)
            peak = std::max(peak, std::fabs(v));
        return peak;
    }
};
TEST_F(InstrumentLiveTest, SilentSourcesAndInstrumentGainCanBeRaisedWithoutRetrigger) {
    ins.osc[1].type = OscType::Sample;
    ins.osc[1].level = 0;
    ins.osc_mix = 1;
    ins.trim_gain = 0;
    ref.gain_mul = .8f;
    zone.gain = .25f;
    auto p = TriggerParams();
    zone.gain = .5f;
    auto secondary = TriggerParams(60, 1);
    PairOscillatorTrigger(p, secondary);
    WaveX::Sequencer::ParamLock gain{WaveX::Protocol::PARAM_GAIN, 16384};
    ApplyParamLocks(p, &gain, 1);
    vm.Trigger(p);
    EXPECT_FLOAT_EQ(Render(), 0);
    const auto frame = vm.GetVoice(0).phase.Frame();
    const auto second_frame = vm.GetVoice(0).secondary.phase.Frame();
    const auto age = vm.GetVoice(0).age;
    ins.trim_gain = .5f;
    ins.osc_mix = .25f;
    ins.osc[0].level = .8f;
    ins.osc[1].level = .6f;
    Live();
    const auto& v = vm.GetVoice(0);
    EXPECT_EQ(v.phase.Frame(), frame);
    EXPECT_EQ(v.secondary.phase.Frame(), second_frame);
    EXPECT_EQ(v.age, age);
    EXPECT_NEAR(v.gain, .25f, 1e-6);
    EXPECT_NEAR(v.source_level, .12f, 1e-6);
    EXPECT_NEAR(v.secondary.source_level, .06f, 1e-6);
    EXPECT_GT(Render(), .005f);
    ins.trim_gain = 0;
    Live();
    EXPECT_FLOAT_EQ(Render(), 0);
    ins.trim_gain = .5f;
    Live();
    EXPECT_GT(Render(), .005f);
    EXPECT_EQ(v.age, age);
}
TEST_F(InstrumentLiveTest, KeyTrackingAndIndependentTuningKeepThePlayedNoteAndCursor) {
    ins.osc[0].keytrack = 0;
    ins.osc[0].coarse_tune = 12;
    zone.coarse_tune = 12;
    auto p = TriggerParams(72);
    ins.osc[1].type = OscType::Sample;
    zone.root_note = 48;
    zone.coarse_tune = -12;
    auto second = TriggerParams(72, 1);
    PairOscillatorTrigger(p, second);
    vm.Trigger(p);
    Render();
    const auto frame = vm.GetVoice(0).phase.Frame();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).base_increment, 4);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).secondary.base_increment, 2);
    ins.osc[0].keytrack = 1;
    ins.osc[1].coarse_tune = -12;
    Live();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).base_increment, 8);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).secondary.base_increment, 1);
    EXPECT_EQ(vm.GetVoice(0).phase.Frame(), frame);
    ins.mode = InstrumentMode::Drum;
    Live();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).base_increment, 4);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).secondary.base_increment, .25f);
}
TEST_F(InstrumentLiveTest, ZonePanAndLocksSurviveOtherControlsAndOtherTracks) {
    zone.pan = .2f;
    ins.trim_pan = .7f;
    auto p = TriggerParams();
    vm.Trigger(p);
    p.track = 1;
    vm.Trigger(p);
    ins.trim_gain = .3f;
    Live();
    EXPECT_NEAR(vm.GetVoice(0).pan, .4f, 1e-6);
    EXPECT_NEAR(vm.GetVoice(1).gain, 1.f, 1e-6);
    p.track = 0;
    WaveX::Sequencer::ParamLock lock{WaveX::Protocol::PARAM_PAN, 65535};
    ApplyParamLocks(p, &lock, 1);
    vm.Trigger(p);
    ins.trim_pan = 0;
    Live();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).pan, 0);
    EXPECT_FLOAT_EQ(vm.GetVoice(2).pan, 1);
}
TEST_F(InstrumentLiveTest, ModulatorUpdatesPreserveEnvelopeAndLfoPhases) {
    ins.env[1] = {1, 0, 1, .1f};
    ins.env[2] = {1, 0, 1, .1f};
    vm.Trigger(TriggerParams(72));
    vm.TickModulation({}, {}, 480);
    const auto& v = vm.GetVoice(0);
    const float env2 = v.env2.Level(), env3 = v.env3.Level(), phase = v.lfo[1].Phase();
    ins.env[1].attack_s = .1f;
    ins.env[2].attack_s = .2f;
    ins.lfo[1].rate_hz = 2;
    ins.lfo[1].pitch_follow = 1;
    Live();
    EXPECT_FLOAT_EQ(v.env2.Level(), env2);
    EXPECT_FLOAT_EQ(v.env3.Level(), env3);
    EXPECT_FLOAT_EQ(v.lfo[1].Phase(), phase);
    vm.TickModulation({}, {}, 480);
    EXPECT_NEAR(v.env2.Level(), env2 + .1f, 1e-6);
    EXPECT_NEAR(v.env3.Level(), env3 + .05f, 1e-6);
    EXPECT_NEAR(v.lfo[1].Phase(), phase + .04f, 1e-5);
    vm.ReleaseTrack(72, 0);
    ins.env[1].release_s = 600;
    ins.env[2].release_s = 600;
    Live();
    const float releasing = v.env2.Level();
    vm.TickModulation({}, {}, 48);
    EXPECT_NEAR(v.env2.Level(), releasing - .01f, 1e-5);
}
TEST_F(InstrumentLiveTest, PreparedMapRetainsActualNoteWhenKeyTrackingStartsDisabled) {
    ins.osc[0].keytrack = 0;
    ins.osc[0].zones[0] = zone;
    SampleResolver resolver{
        &ref, [](const void* c, uint16_t) { return *static_cast<const SampleRef*>(c); }};
    SequencerVoiceMap map;
    map.PrepareTrack(0, ins, resolver);
    VoiceTriggerParams p[4];
    ASSERT_EQ(map.Resolve(0, 72, 127, p), 1);
    vm.Trigger(p[0]);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).increment, 1);
    ins.osc[0].keytrack = 1;
    Live();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).increment, 2);
}
}  // namespace

TEST_F(InstrumentLiveTest, FilterModeFollowsOnlyItsInstrumentAndRevertPreservesCursor) {
    using Mode = SvfFilter::Mode;
    ins.filter.cutoff_hz = 1000;
    auto first = TriggerParams();
    auto other = first;
    other.track = 1;
    vm.Trigger(first);
    vm.Trigger(other);
    Render();
    const auto frame = vm.GetVoice(0).phase.Frame();
    InstrumentSoundUndo undo;
    undo.Capture(ins);
    ins.filter.type = WaveX::Protocol::INST_FILTER_HP;
    Live();
    EXPECT_EQ(vm.GetVoice(0).filter.GetMode(), Mode::HighPass);
    EXPECT_EQ(vm.GetVoice(1).filter.GetMode(), Mode::LowPass);
    EXPECT_EQ(vm.GetVoice(0).phase.Frame(), frame);
    EXPECT_EQ(TriggerParams().filter_mode, Mode::HighPass);
    ASSERT_TRUE(undo.Revert(ins));
    Live();
    EXPECT_EQ(vm.GetVoice(0).filter.GetMode(), Mode::LowPass);
    EXPECT_EQ(vm.GetVoice(0).phase.Frame(), frame);
}

TEST_F(InstrumentLiveTest, ResonanceRouteRemovalAndLiveEditsUseTheOwningNotesBase) {
    ins.filter.resonance = .2f;
    vm.Trigger(TriggerParams());
    auto other = TriggerParams();
    other.track = 1;
    vm.Trigger(other);
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_RESONANCE, 16384, CURVE_LINEAR, 0};
    ModSlotResolver routes{&ins, [](const void* c, uint8_t track) -> const ModSlot* {
                               return track == 0 ? static_cast<const Instrument*>(c)->mod_slots
                                                 : nullptr;
                           }};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_NEAR(vm.GetVoice(0).filter.Resonance(), .7f, .0001f);
    EXPECT_FLOAT_EQ(vm.GetVoice(1).filter.Resonance(), .2f);
    const auto frame = vm.GetVoice(0).phase.Frame();
    ins.filter.resonance = .8f;
    Live();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.Resonance(), 1);
    EXPECT_EQ(vm.GetVoice(0).phase.Frame(), frame);
    ins.mod_slots[0] = {};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.Resonance(), .8f);
    EXPECT_FLOAT_EQ(vm.GetVoice(1).filter.Resonance(), .2f);
}
TEST_F(InstrumentLiveTest, ResonanceLockAndStolenVoiceKeepIndependentBaseAndModulation) {
    auto p = TriggerParams();
    WaveX::Sequencer::ParamLock lock{WaveX::Protocol::PARAM_FILTER_RESONANCE, 32768};
    ApplyParamLocks(p, &lock, 1);
    for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i)
        vm.Trigger(p);
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_RESONANCE, -32767, CURVE_LINEAR, 0};
    ModSlotResolver routes{
        &ins, [](const void* c, uint8_t) { return static_cast<const Instrument*>(c)->mod_slots; }};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.Resonance(), 0);
    ins.filter.resonance = .9f;
    Live();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.Resonance(), 0);
    ins.mod_slots[0] = {};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_NEAR(vm.GetVoice(0).filter.Resonance(), .5f, .0001f);
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_RESONANCE, -32767, CURVE_LINEAR, 0};
    vm.TickModulation(routes, {}, 48);
    Render();
    vm.Trigger(TriggerParams());
    EXPECT_FLOAT_EQ(vm.GetVoice(0).mod_resonance_offset, 0);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.Resonance(), .9f);
}
