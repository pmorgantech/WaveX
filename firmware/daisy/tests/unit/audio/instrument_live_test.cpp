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
TEST_F(InstrumentLiveTest, TrackPanAppliesToHeldAndFutureNotesWithoutCompounding) {
    zone.pan = .8f;
    ins.trim_pan = .9f;  // unclamped base 1.2; Track pan offsets this before clamping
    vm.Trigger(TriggerParams());
    VoiceLiveParams live;
    live.track = 0;
    live.pan = .1f;
    PrepareInstrumentLive(ins, live);
    vm.ApplyLiveParams(live);
    vm.Trigger(TriggerParams());
    EXPECT_NEAR(vm.GetVoice(0).pan, .8f, 1e-6f);
    EXPECT_NEAR(vm.GetVoice(1).pan, .8f, 1e-6f);
    vm.ApplyLiveParams(live);
    EXPECT_NEAR(vm.GetVoice(0).pan, .8f, 1e-6f);
    auto p = TriggerParams();
    p.track = 1;
    vm.Trigger(p);
    EXPECT_FLOAT_EQ(vm.GetVoice(2).pan, 1.f);
    p.track = 0;
    p.preview = true;
    vm.Trigger(p);
    EXPECT_FLOAT_EQ(vm.GetVoice(3).pan, 1.f);
    p.preview = false;
    WaveX::Sequencer::ParamLock lock{WaveX::Protocol::PARAM_PAN, 0};
    ApplyParamLocks(p, &lock, 1);
    vm.Trigger(p);
    vm.ApplyLiveParams(live);
    EXPECT_FLOAT_EQ(vm.GetVoice(4).pan, 0.f);
    vm.Init(48000);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).pan, .5f);
    vm.Trigger(TriggerParams());
    EXPECT_FLOAT_EQ(vm.GetVoice(0).pan, 1.f);
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
TEST_F(InstrumentLiveTest, MonoChangesNextNotesAndRevertsWithoutReallocatingHeldNotes) {
    ref.channels = 2;
    ref.frames = pcm.size() / 2;
    auto first = TriggerParams();
    EXPECT_FALSE(first.mono);
    vm.Trigger(first);
    ASSERT_EQ(vm.ActiveChannelCount(), 2);
    InstrumentSoundUndo undo;
    undo.Capture(ins);
    ins.osc[0].mono = true;
    Live();
    EXPECT_EQ(vm.ActiveChannelCount(), 2);
    auto second = TriggerParams(62);
    EXPECT_TRUE(second.mono);
    vm.Trigger(second);
    EXPECT_EQ(vm.ActiveChannelCount(), 3);
    ASSERT_TRUE(undo.Revert(ins));
    EXPECT_FALSE(ins.osc[0].mono);
    Live();
    EXPECT_EQ(vm.ActiveChannelCount(), 3);
    EXPECT_FALSE(TriggerParams().mono);
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

TEST_F(InstrumentLiveTest, FilterTopologyFollowsTheInstrumentOnHeldAndNextNotes) {
    auto first = TriggerParams();
    auto other = first;
    other.track = 1;
    vm.Trigger(first);
    vm.Trigger(other);
    Render();
    const auto frame = vm.GetVoice(0).phase.Frame();
    InstrumentSoundUndo undo;
    undo.Capture(ins);
    ins.filter.topology = WaveX::Protocol::INST_FILTER_TOPOLOGY_LADDER;
    Live();
    EXPECT_EQ(vm.GetVoice(0).filter.GetTopology(), FilterTopology::Ladder);
    EXPECT_EQ(vm.GetVoice(1).filter.GetTopology(), FilterTopology::WaveXSvf);
    EXPECT_EQ(vm.GetVoice(0).phase.Frame(), frame);
    EXPECT_EQ(TriggerParams().filter_topology, FilterTopology::Ladder);
    vm.Trigger(TriggerParams(62));
    EXPECT_EQ(vm.GetVoice(2).filter.GetTopology(), FilterTopology::Ladder);
    ASSERT_TRUE(undo.Revert(ins));
    Live();
    EXPECT_EQ(vm.GetVoice(0).filter.GetTopology(), FilterTopology::WaveXSvf);
    EXPECT_EQ(vm.GetVoice(0).phase.Frame(), frame);
    EXPECT_EQ(TriggerParams().filter_topology, FilterTopology::WaveXSvf);
}

TEST_F(InstrumentLiveTest, FilterSlopeAndDriveFollowTheInstrumentOnHeldAndNextNotes) {
    auto first = TriggerParams();
    auto other = first;
    other.track = 1;
    vm.Trigger(first);
    vm.Trigger(other);
    Render();
    EXPECT_EQ(vm.GetVoice(0).filter.GetConfig().slope, SvfFilter::Slope::Db12);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.GetConfig().drive, 0.0f);
    InstrumentSoundUndo undo;
    undo.Capture(ins);
    ins.filter.slope = WaveX::Protocol::INST_FILTER_SLOPE_24;
    ins.filter.drive = 0.5f;
    Live();
    EXPECT_EQ(vm.GetVoice(0).filter.GetConfig().slope, SvfFilter::Slope::Db24);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.GetConfig().drive, 0.5f);
    EXPECT_EQ(vm.GetVoice(1).filter.GetConfig().slope, SvfFilter::Slope::Db12);
    EXPECT_FLOAT_EQ(vm.GetVoice(1).filter.GetConfig().drive, 0.0f);
    EXPECT_EQ(TriggerParams().filter_config.slope, SvfFilter::Slope::Db24);
    EXPECT_FLOAT_EQ(TriggerParams().filter_config.drive, 0.5f);
    vm.Trigger(TriggerParams(62));
    EXPECT_EQ(vm.GetVoice(2).filter.GetConfig().slope, SvfFilter::Slope::Db24);
    ASSERT_TRUE(undo.Revert(ins));
    Live();
    EXPECT_EQ(vm.GetVoice(0).filter.GetConfig().slope, SvfFilter::Slope::Db12);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).filter.GetConfig().drive, 0.0f);
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

TEST_F(InstrumentLiveTest, IndependentPitchRoutesKeepTuningCursorsAndTrackOwnership) {
    ins.osc[1].type = OscType::Sample;
    ins.osc[1].coarse_tune = 12;
    auto p = TriggerParams();
    PairOscillatorTrigger(p, TriggerParams(60, 1));
    vm.Trigger(p);
    p.track = 1;
    vm.Trigger(p);
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_OSC1_PITCH, 32767, CURVE_LINEAR, 0};
    ins.mod_slots[1] = {SRC_VELOCITY, DEST_OSC2_PITCH, -32767, CURVE_LINEAR, 0};
    ModSlotResolver routes{&ins, [](const void* c, uint8_t track) -> const ModSlot* {
                               return track == 0 ? static_cast<const Instrument*>(c)->mod_slots
                                                 : nullptr;
                           }};
    vm.TickModulation(routes, {}, 48);
    const auto& v = vm.GetVoice(0);
    EXPECT_EQ(v.phase.Frame(), 0u);
    Render();
    const float up = std::pow(2.f, 2.f / 12);
    EXPECT_NEAR(v.increment, up, 1e-6);
    EXPECT_NEAR(v.secondary.increment, 2.f / up, 1e-6);
    EXPECT_FLOAT_EQ(vm.GetVoice(1).increment, 1);
    EXPECT_FLOAT_EQ(vm.GetVoice(1).secondary.increment, 2);
    const auto frame = v.phase.Frame();
    const auto second = v.secondary.phase.Frame();
    const auto age = v.age;
    ins.osc[0].coarse_tune = 12;
    Live();
    EXPECT_EQ(v.phase.Frame(), frame);
    EXPECT_EQ(v.secondary.phase.Frame(), second);
    Render();
    EXPECT_NEAR(v.increment, 2 * up, 1e-6);
    EXPECT_NEAR(v.secondary.increment, 2 / up, 1e-6);
    ins.mod_slots[0] = {};
    ins.mod_slots[1] = {};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_FLOAT_EQ(v.increment, 2);
    EXPECT_FLOAT_EQ(v.secondary.increment, 2);
    EXPECT_GT(v.phase.Frame(), frame);
    EXPECT_GT(v.secondary.phase.Frame(), second);
    EXPECT_EQ(v.age, age);
}
TEST_F(InstrumentLiveTest, OscillatorTwoOnlyAndPreviewSourcesUseTheirActualIdentity) {
    ins.osc[1].type = OscType::Sample;
    vm.Trigger(TriggerParams(60, 1));
    auto p = TriggerParams();
    p.oscillator = 0xFF;
    vm.Trigger(p);
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_OSC1_PITCH, 32767, CURVE_LINEAR, 0};
    ins.mod_slots[1] = {SRC_VELOCITY, DEST_OSC2_PITCH, -32767, CURVE_LINEAR, 0};
    ins.mod_slots[2] = {SRC_VELOCITY, DEST_PITCH, 32767, CURVE_LINEAR, 0};
    ModSlotResolver routes{
        &ins, [](const void* c, uint8_t) { return static_cast<const Instrument*>(c)->mod_slots; }};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_EQ(vm.GetVoice(0).oscillator, 1);
    EXPECT_EQ(vm.GetVoice(0).secondary.sample, nullptr);
    EXPECT_NEAR(vm.GetVoice(0).increment, 1, 1e-6);
    EXPECT_NEAR(vm.GetVoice(1).increment, std::pow(2.f, 2.f / 12), 1e-6);
}
TEST_F(InstrumentLiveTest, PitchLockAndStolenVoiceDoNotInheritOscillatorModulation) {
    auto p = TriggerParams();
    WaveX::Sequencer::ParamLock lock{WaveX::Protocol::PARAM_PITCH, 49151};
    ApplyParamLocks(p, &lock, 1);
    for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i)
        vm.Trigger(p);
    const float locked = vm.GetVoice(0).increment;
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_OSC1_PITCH, 32767, CURVE_LINEAR, 0};
    ModSlotResolver routes{
        &ins, [](const void* c, uint8_t) { return static_cast<const Instrument*>(c)->mod_slots; }};
    vm.TickModulation(routes, {}, 48);
    Render();
    const float up = std::pow(2.f, 2.f / 12);
    EXPECT_NEAR(vm.GetVoice(0).increment, locked * up, 1e-6);
    VoiceLiveParams live;
    live.track = 0;
    live.pitch_semitones = -12;
    vm.ApplyLiveParams(live);
    Render();
    EXPECT_NEAR(vm.GetVoice(0).increment, locked * up, 1e-6);
    ins.mod_slots[0] = {};
    vm.TickModulation(routes, {}, 48);
    Render();
    EXPECT_FLOAT_EQ(vm.GetVoice(0).increment, locked);
    ins.mod_slots[0] = {SRC_VELOCITY, DEST_OSC1_PITCH, 32767, CURVE_LINEAR, 0};
    vm.TickModulation(routes, {}, 48);
    Render();
    vm.Trigger(TriggerParams());
    EXPECT_FLOAT_EQ(vm.GetVoice(0).mod_oscillator_pitch_mul[0], 1);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).mod_oscillator_pitch_mul[1], 1);
    EXPECT_FLOAT_EQ(vm.GetVoice(0).increment, .5f);
}

TEST_F(InstrumentLiveTest, MixModulationRecoversSilentOscillatorAndHonorsLiveBase) {
    ins.osc[1].type = OscType::Sample;
    ins.osc[0].level = .8f;
    ins.osc[1].level = .6f;
    ins.osc_mix = 0;
    zone.gain = .5f;
    auto params = TriggerParams();
    auto second = TriggerParams(60, 1);
    PairOscillatorTrigger(params, second);
    vm.Trigger(params);
    ModSlot slots[kMaxModSlots]{};
    slots[0] = {SRC_MODWHEEL, DEST_OSC_MIX, 32767, 0, 0};
    ModSlotResolver resolver{slots,
                             [](const void* p, uint8_t) { return static_cast<const ModSlot*>(p); }};
    ModSources sources;
    sources.modwheel = 1;
    vm.TickModulation(resolver, sources, 48);
    const auto& voice = vm.GetVoice(0);
    EXPECT_FLOAT_EQ(voice.SourceLevel(voice), 0);
    EXPECT_NEAR(voice.SourceLevel(voice.secondary), .3f, 1e-6);
    ins.osc_mix = .25f;
    ins.osc[1].level = .2f;
    Live();
    sources.modwheel = .25f;
    vm.TickModulation(resolver, sources, 48);
    EXPECT_NEAR(voice.SourceLevel(voice), .2f, 1e-6);
    EXPECT_NEAR(voice.SourceLevel(voice.secondary), .05f, 1e-6);
    slots[0] = {};
    vm.TickModulation(resolver, sources, 48);
    EXPECT_NEAR(voice.SourceLevel(voice), .3f, 1e-6);
    EXPECT_NEAR(voice.SourceLevel(voice.secondary), .025f, 1e-6);
    EXPECT_GT(Render(), 0);
}

TEST_F(InstrumentLiveTest, RateRoutesHaveOneBlockDelayAndClearingPreservesPhase) {
    ins.lfo[0].wave = 3;  // square is initially +1, including self-modulation
    ins.lfo[0].rate_hz = 1;
    ins.lfo[1].rate_hz = 2;
    vm.Trigger(TriggerParams());
    ModSlot slots[kMaxModSlots]{};
    slots[0] = {SRC_LFO_VOICE, DEST_LFO1_RATE, 32767, 0, 0};
    slots[1] = {SRC_LFO_VOICE, DEST_LFO2_RATE, 32767, 0, 0};
    ModSlotResolver resolver{slots,
                             [](const void* p, uint8_t) { return static_cast<const ModSlot*>(p); }};
    const auto& voice = vm.GetVoice(0);
    vm.TickModulation(resolver, {}, 48);
    EXPECT_NEAR(voice.lfo[0].Phase(), .001f, .00001f);
    EXPECT_NEAR(voice.lfo[1].Phase(), .002f, .00001f);
    vm.TickModulation(resolver, {}, 48);
    EXPECT_NEAR(voice.lfo[0].Phase(), .017f, .00001f);
    EXPECT_NEAR(voice.lfo[1].Phase(), .034f, .00001f);
    slots[0] = slots[1] = {};
    vm.TickModulation(resolver, {}, 48);  // previously published rate advances once
    const auto phase = voice.lfo[0].Phase();
    vm.TickModulation(resolver, {}, 48);
    EXPECT_NEAR(voice.lfo[0].Phase(), phase + .001f, .00001f);
}
TEST_F(InstrumentLiveTest, MixRouteUsesLoneOscillatorTwoIdentityAndPreservesZeroLevel) {
    ins.osc[1].type = OscType::Sample;
    ins.osc[1].level = .6f;
    ins.osc_mix = 0;
    zone.gain = .5f;
    vm.Trigger(TriggerParams(60, 1));
    ModDestinations mod;
    mod.oscillator_mix_offset = 1;
    auto& voice = const_cast<Voice&>(vm.GetVoice(0));
    voice.SetBlockModulation(mod);
    EXPECT_NEAR(voice.SourceLevel(voice), .6f, 1e-6);
    EXPECT_NEAR(voice.gain, .5f, 1e-6);
    ins.osc[1].level = 0;
    Live();
    EXPECT_EQ(voice.SourceLevel(voice), 0);
}
