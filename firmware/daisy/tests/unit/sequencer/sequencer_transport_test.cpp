#include "sequencer/sequencer_transport.hpp"

#include <gtest/gtest.h>

#include <vector>

using WaveX::Sequencer::SequencerTransport;
using WaveX::Sequencer::StepScale;
using WaveX::Sequencer::SyncLockState;
using WaveX::Sequencer::TriggerEvent;
using namespace WaveX::Protocol;

namespace {

std::vector<TriggerEvent> RunTicks(SequencerTransport& t, int ticks) {
    std::vector<TriggerEvent> all;
    TriggerEvent buf[64];
    for (int i = 0; i < ticks; ++i) {
        size_t n = t.Tick(buf, 64);
        for (size_t j = 0; j < n; ++j)
            all.push_back(buf[j]);
    }
    return all;
}

// Feeds a clean MIDI clock stream (interval = one 24-PPQN tick at `bpm`) into
// the transport, ticking the control clock the right number of times between
// events. Mirrors the harness in tempo_follower_test.cpp but drives through
// the transport's OnMidiClock path.
void FeedMidiClocks(SequencerTransport& t, int count, double bpm) {
    const double period_us = 2.5e6 / bpm;
    double budget_us = 0.0;
    for (int i = 0; i < count; ++i) {
        budget_us += period_us;
        int ticks = static_cast<int>(budget_us / 1000.0);
        budget_us -= ticks * 1000.0;
        TriggerEvent buf[64];
        for (int k = 0; k < ticks; ++k)
            t.Tick(buf, 64);
        t.OnMidiClock(MidiClockEventMessage(
            MIDI_CLK_TICK, 0, static_cast<uint16_t>(i), static_cast<uint32_t>(period_us + 0.5), 0));
    }
}

SequencerTransport MakeTransport() {
    SequencerTransport t;
    t.Init(48000, 48);
    return t;
}

}  // namespace

// ---- Pattern edits ----

TEST(SequencerTransportTest, SetStepOpEnablesStepWithVelocity) {
    SequencerTransport t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 2, 5, 1, 99, 0));

    const auto& step = t.pattern().tracks[2].steps[5];
    EXPECT_TRUE(step.on);
    EXPECT_EQ(step.velocity, 99);
}

TEST(SequencerTransportTest, ToggleStepFlipsOnOff) {
    SequencerTransport t = MakeTransport();
    EXPECT_FALSE(t.pattern().tracks[0].steps[0].on);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TOGGLE_STEP, 0, 0, 0, 0, 0));
    EXPECT_TRUE(t.pattern().tracks[0].steps[0].on);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TOGGLE_STEP, 0, 0, 0, 0, 0));
    EXPECT_FALSE(t.pattern().tracks[0].steps[0].on);
}

TEST(SequencerTransportTest, VelocityAndProbabilityAreClamped) {
    SequencerTransport t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 0, 0, 1, 500 /*vel*/, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].velocity, 127);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP_PROB, 0, 0, 200 /*prob*/, 0, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].probability, 100);
}

TEST(SequencerTransportTest, MicroOffsetAndRetrigOpAppliesSignedOffset) {
    SequencerTransport t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP_MICRO, 1, 3, 2 /*count*/, 8 /*rate*/, -5));
    const auto& s = t.pattern().tracks[1].steps[3];
    EXPECT_EQ(s.retrig_count, 2);
    EXPECT_EQ(s.retrig_rate_ticks, 8);
    EXPECT_EQ(s.micro_offset, -5);
}

TEST(SequencerTransportTest, PatternScopedOpsAreClamped) {
    SequencerTransport t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 999, 0));
    EXPECT_EQ(t.pattern().length, WaveX::Sequencer::kMaxSteps);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_SWING, 0, 0, 200, 0, 0));
    EXPECT_EQ(t.pattern().swing, 75);
    t.ApplyPatternOp(SeqPatternOpMessage(
        SEQ_OP_PATTERN_SCALE, 0, 0, static_cast<uint8_t>(StepScale::EighthTriplet), 0, 0));
    EXPECT_EQ(t.pattern().scale, StepScale::EighthTriplet);
}

TEST(SequencerTransportTest, TrackMuteOpTogglesEnabled) {
    SequencerTransport t = MakeTransport();
    EXPECT_TRUE(t.pattern().tracks[4].enabled);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TRACK_MUTE, 4, 0, 0 /*disable*/, 0, 0));
    EXPECT_FALSE(t.pattern().tracks[4].enabled);
}

TEST(SequencerTransportTest, OutOfRangeEditsAreSilentNoOps) {
    SequencerTransport t = MakeTransport();
    // track 99 / step 99 are out of range - must not crash or corrupt.
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 99, 99, 1, 100, 0));
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TRACK_MUTE, 200, 0, 0, 0, 0));
    // Nothing enabled anywhere.
    for (uint8_t tr = 0; tr < WaveX::Sequencer::kMaxTracks; ++tr)
        for (uint8_t st = 0; st < WaveX::Sequencer::kMaxSteps; ++st)
            EXPECT_FALSE(t.pattern().tracks[tr].steps[st].on);
}

TEST(SequencerTransportTest, ParamLockSetOverwriteAndClear) {
    SequencerTransport t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_PARAM_LOCK, 0, 0, 5 /*param*/, 200, 0));
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_PARAM_LOCK, 0, 0, 5 /*param*/, 300, 0));
    // Same param id overwrites in place, not a second slot.
    const auto& s = t.pattern().tracks[0].steps[0];
    EXPECT_EQ(s.param_locks[0].param_id, 5);
    EXPECT_EQ(s.param_locks[0].value, 300);
    EXPECT_EQ(s.param_locks[1].param_id, 0);

    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_CLEAR_PARAM_LOCKS, 0, 0, 0, 0, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].param_locks[0].param_id, 0);
}

// ---- Internal-clock transport ----

TEST(SequencerTransportTest, InternalPlayStartsSchedulerImmediately) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Quarter;
    t.pattern().tracks[0].steps[0].on = true;

    // 120 BPM, internal.
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));
    EXPECT_TRUE(t.IsPlaying());
    EXPECT_FALSE(t.UsingMidiSync());

    auto events = RunTicks(t, 2500);  // ~5 beats worth
    ASSERT_GE(events.size(), 4u);
    // First hit at frame 0 (downbeat fires immediately).
    EXPECT_EQ(events[0].frame, 0u);
    EXPECT_EQ(events[1].frame, 24000u);  // one beat = 24000 frames @120bpm/48k
}

TEST(SequencerTransportTest, StopHaltsPlayback) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Quarter;
    t.pattern().tracks[0].steps[0].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));
    RunTicks(t, 100);
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));
    EXPECT_FALSE(t.IsPlaying());
    auto events = RunTicks(t, 2000);
    EXPECT_TRUE(events.empty());
}

TEST(SequencerTransportTest, TempoFromTransportMessageDrivesTiming) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Quarter;
    t.pattern().tracks[0].steps[0].on = true;

    // 60 BPM => one beat = 48000 frames.
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 6000, 0));
    EXPECT_DOUBLE_EQ(t.TempoBpm(), 60.0);
    auto events = RunTicks(t, 3000);
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0].frame, 0u);
    EXPECT_EQ(events[1].frame, 48000u);
}

// ---- Playhead feedback ----

TEST(SequencerTransportTest, PlayheadReportsStepAndBpm) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 4;
    t.pattern().scale = StepScale::Sixteenth;
    for (int i = 0; i < 4; ++i)
        t.pattern().tracks[0].steps[i].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));

    SeqPlayheadMessage ph0 = t.BuildPlayhead();
    EXPECT_EQ(ph0.playing, 1);
    EXPECT_EQ(ph0.sync_state, 0);  // internal
    EXPECT_EQ(ph0.measured_bpm_x100, 12000);

    // Advance ~2.5 sixteenth-notes (each = 24 ticks = 125 blocks @120/48k).
    RunTicks(t, 300);
    SeqPlayheadMessage ph = t.BuildPlayhead();
    EXPECT_EQ(ph.playing, 1);
    // Somewhere within the 4-step pattern.
    EXPECT_LT(ph.step, 4);
}

// ---- MIDI-slave transport ----

TEST(SequencerTransportTest, MidiPlayArmsAndStartsOnMidiStart) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Quarter;
    t.pattern().tracks[0].steps[0].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0));
    EXPECT_TRUE(t.UsingMidiSync());
    EXPECT_TRUE(t.IsArmed());
    EXPECT_FALSE(t.IsPlaying());  // not started until MIDI START

    // A few ticks with no clock: still not playing.
    RunTicks(t, 10);
    EXPECT_FALSE(t.IsPlaying());

    // MIDI START begins playback.
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_START, 0, 0, 0, 0));
    EXPECT_TRUE(t.IsPlaying());
    EXPECT_FALSE(t.IsArmed());
}

TEST(SequencerTransportTest, MidiClockLocksAndTracksTempo) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Sixteenth;
    t.pattern().tracks[0].steps[0].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0));
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_START, 0, 0, 0, 0));

    // Feed a clean 140 BPM clock; follower should lock and report ~140.
    FeedMidiClocks(t, 24, 140.0);

    EXPECT_EQ(t.SyncState(), SyncLockState::Locked);
    SeqPlayheadMessage ph = t.BuildPlayhead();
    EXPECT_EQ(ph.sync_state, 2);  // locked
    EXPECT_NEAR(ph.measured_bpm_x100 / 100.0, 140.0, 1.0);
}

TEST(SequencerTransportTest, MidiStopHaltsScheduler) {
    SequencerTransport t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Sixteenth;
    t.pattern().tracks[0].steps[0].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0));
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_START, 0, 0, 0, 0));
    FeedMidiClocks(t, 8, 120.0);
    EXPECT_TRUE(t.IsPlaying());

    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_STOP, 0, 0, 0, 0));
    EXPECT_FALSE(t.IsPlaying());
}

// ---- MIDI CC forwarding ----

TEST(SequencerTransportTest, MidiCcIsRecorded) {
    SequencerTransport t = MakeTransport();
    EXPECT_EQ(t.CcCount(), 0u);
    t.OnMidiCc(MidiCcMessage(1 /*modwheel*/, 77, 3));
    EXPECT_EQ(t.CcCount(), 1u);
    EXPECT_EQ(t.LastCc(), 1);
    EXPECT_EQ(t.LastCcValue(), 77);
    EXPECT_EQ(t.LastCcChannel(), 3);
}

// ---- Mode fields ----

TEST(SequencerTransportTest, InputModeStoredFromTransport) {
    SequencerTransport t = MakeTransport();
    t.ApplyTransport(SeqTransportMessage(
        SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0));
    EXPECT_EQ(t.InputMode(), SEQ_INPUT_LIVE_RECORD);
}
