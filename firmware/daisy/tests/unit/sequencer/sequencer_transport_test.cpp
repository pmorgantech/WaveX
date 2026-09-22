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

struct TestTransport : SequencerTransport {
    TestTransport() { Init(48000, 48); }
};
TestTransport MakeTransport() {
    return TestTransport{};
}

}  // namespace

// ---- Pattern edits ----

TEST(SequencerTransportTest, EditsPreserveFiredRetriggersAndRespectNewBoundaries) {
    for (int edit = 0; edit < 4; ++edit) {
        auto t = MakeTransport();
        auto& step = t.pattern().tracks[0].steps[0];
        step.on = true;
        step.retrig_count = 2;
        step.retrig_rate_ticks = 4;
        t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, 0, 0, 12000, 0});
        if (edit == 0)
            t.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 1, 4, 65, 0, 0});
        else if (edit == 1)
            t.ApplyPatternOp({SEQ_OP_SET_STEP_MICRO, 0, 1, 0, 0, -18});
        else if (edit == 2)
            t.ApplyPatternOp({SEQ_OP_TRACK_MUTE, 0, 0, 0, 0, 0});
        else
            t.ApplyPatternOp({SEQ_OP_SET_MELODIC, 0, 0, 1, 0, 0});
        const auto events = RunTicks(t, 60);
        std::vector<uint64_t> retrigger_frames;
        for (const auto& event: events)
            if (event.track == 0 && event.is_retrig)
                retrigger_frames.push_back(event.frame);
        const std::vector<uint64_t> expected = edit == 0   ? std::vector<uint64_t>{1000, 2000}
                                               : edit == 1 ? std::vector<uint64_t>{1000}
                                                           : std::vector<uint64_t>{};
        EXPECT_EQ(retrigger_frames, expected) << "edit " << edit;
    }
}

TEST(SequencerTransportTest, PlaybackRowsIgnoreUncommittedEditorChanges) {
    auto t = MakeTransport();
    t.pattern().tracks[0].melodic = true;
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, 0, 0, 12000, 0});
    t.ApplyPatternOp({SEQ_OP_SET_MELODIC, 0, 0, 0, 0, 0});
    EXPECT_FALSE(t.pattern().tracks[0].melodic);
    EXPECT_TRUE(t.PlaybackPattern().tracks[0].melodic);
    RunTicks(t, 1);
    EXPECT_FALSE(t.PlaybackPattern().tracks[0].melodic);
}

TEST(SequencerTransportTest, SetStepOpEnablesStepWithVelocity) {
    auto t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 2, 5, 1, 99, 0));

    const auto& step = t.pattern().tracks[2].steps[5];
    EXPECT_TRUE(step.on);
    EXPECT_EQ(step.velocity, 99);
}

TEST(SequencerTransportTest, ToggleStepFlipsOnOff) {
    auto t = MakeTransport();
    EXPECT_FALSE(t.pattern().tracks[0].steps[0].on);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TOGGLE_STEP, 0, 0, 0, 0, 0));
    EXPECT_TRUE(t.pattern().tracks[0].steps[0].on);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TOGGLE_STEP, 0, 0, 0, 0, 0));
    EXPECT_FALSE(t.pattern().tracks[0].steps[0].on);
}

TEST(SequencerTransportTest, VelocityAndProbabilityAreClamped) {
    auto t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 0, 0, 1, 500 /*vel*/, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].velocity, 127);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP_PROB, 0, 0, 200 /*prob*/, 0, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].probability, 100);
}

TEST(SequencerTransportTest, MicroOffsetAndRetrigOpAppliesSignedOffset) {
    auto t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP_MICRO, 1, 3, 2 /*count*/, 8 /*rate*/, -5));
    const auto& s = t.pattern().tracks[1].steps[3];
    EXPECT_EQ(s.retrig_count, 2);
    EXPECT_EQ(s.retrig_rate_ticks, 8);
    EXPECT_EQ(s.micro_offset, -5);
}

TEST(SequencerTransportTest, PatternScopedOpsAreClamped) {
    auto t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 999, 0));
    EXPECT_EQ(t.pattern().length, WaveX::Sequencer::kMaxSteps);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_SWING, 0, 0, 200, 0, 0));
    EXPECT_EQ(t.pattern().swing, 75);
    t.ApplyPatternOp(SeqPatternOpMessage(
        SEQ_OP_PATTERN_SCALE, 0, 0, static_cast<uint8_t>(StepScale::EighthTriplet), 0, 0));
    EXPECT_EQ(t.pattern().scale, StepScale::EighthTriplet);
}

TEST(SequencerTransportTest, TrackMuteOpTogglesEnabled) {
    auto t = MakeTransport();
    EXPECT_TRUE(t.pattern().tracks[4].enabled);
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TRACK_MUTE, 4, 0, 0 /*disable*/, 0, 0));
    EXPECT_FALSE(t.pattern().tracks[4].enabled);
}

TEST(SequencerTransportTest, OutOfRangeEditsAreSilentNoOps) {
    auto t = MakeTransport();
    // track 99 / step 99 are out of range - must not crash or corrupt.
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 99, 99, 1, 100, 0));
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_TRACK_MUTE, 200, 0, 0, 0, 0));
    // Nothing enabled anywhere.
    for (uint8_t tr = 0; tr < WaveX::Sequencer::kMaxTracks; ++tr)
        for (uint8_t st = 0; st < WaveX::Sequencer::kMaxSteps; ++st)
            EXPECT_FALSE(t.pattern().tracks[tr].steps[st].on);
}

TEST(SequencerTransportTest, PatternLengthAndSwingClampAtTheLowEndToo) {
    auto t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 0 /*len*/, 0));
    EXPECT_EQ(t.pattern().length, 1) << "length 0 must clamp to 1, not disable the pattern";
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_SWING, 0, 0, 10 /*swing*/, 0, 0));
    EXPECT_EQ(t.pattern().swing, 50) << "swing below straight must clamp to 50";
}

TEST(SequencerTransportTest, InvalidScaleValueIsIgnored) {
    auto t = MakeTransport();
    const StepScale before = t.pattern().scale;
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_PATTERN_SCALE, 0, 0, 200 /*bogus*/, 0, 0));
    EXPECT_EQ(t.pattern().scale, before) << "an unknown scale byte from the wire must not land";
}

TEST(SequencerTransportTest, RetrigCountIsClampedToMax) {
    auto t = MakeTransport();
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP_MICRO, 0, 0, 200 /*count*/, 8, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].retrig_count, WaveX::Sequencer::kMaxRetrigCount);
}

TEST(SequencerTransportTest, ParamLockSetOverwriteAndClear) {
    auto t = MakeTransport();
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

TEST(SequencerTransportTest, ParamLockIdZeroIsRejected) {
    auto t = MakeTransport();
    // param_id 0 marks a free slot; accepting it from the wire would create
    // an "unused" lock carrying a value.
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_PARAM_LOCK, 0, 0, 0 /*param*/, 123, 0));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].param_locks[0].param_id, 0);
    EXPECT_EQ(t.pattern().tracks[0].steps[0].param_locks[0].value, 0);
}

// A 5th distinct lock on a 4-slot step evicts the OLDEST (slot 0 shifts
// out), per param-locks-and-modulation.md §2 - not the newest, and not a
// silent drop.
TEST(SequencerTransportTest, FifthParamLockEvictsTheOldest) {
    auto t = MakeTransport();
    for (uint8_t id = 2; id <= 5; ++id)
        t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_PARAM_LOCK, 0, 0, id, id * 100, 0));
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_PARAM_LOCK, 0, 0, 6, 600, 0));

    const auto& s = t.pattern().tracks[0].steps[0];
    // Locks 3..6 survive, in shifted order; lock 2 is gone.
    EXPECT_EQ(s.param_locks[0].param_id, 3);
    EXPECT_EQ(s.param_locks[1].param_id, 4);
    EXPECT_EQ(s.param_locks[2].param_id, 5);
    EXPECT_EQ(s.param_locks[3].param_id, 6);
    EXPECT_EQ(s.param_locks[3].value, 600);
}

// ---- Internal-clock transport ----

TEST(SequencerTransportTest, InternalPlayStartsSchedulerImmediately) {
    auto t = MakeTransport();
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

TEST(SequencerTransportTest, RuntimePatternEditTakesEffectAfterTheCurrentStepBoundary) {
    auto t = MakeTransport();
    t.pattern().length = 2;
    t.pattern().tracks[0].steps[0].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));
    ASSERT_EQ(RunTicks(t, 1).size(), 1u);  // step 0 at frame 0

    // This edit arrives during step 0. Its step-1 hit must not appear at the
    // boundary that completes the step currently being scheduled.
    t.ApplyPatternOp(SeqPatternOpMessage(SEQ_OP_SET_STEP, 0, 1, 1, 100, 0));
    EXPECT_TRUE(RunTicks(t, 124).empty());
    EXPECT_TRUE(RunTicks(t, 1).empty()) << "the just-crossed step uses active Pattern";

    // The pending copy committed immediately after that crossing, so the
    // following loop observes it without a partial Pattern read.
    const auto later = RunTicks(t, 250);
    ASSERT_EQ(later.size(), 2u);
    EXPECT_EQ(later[0].step, 0);
    EXPECT_EQ(later[1].step, 1);
}

TEST(SequencerTransportTest, StopHaltsPlayback) {
    auto t = MakeTransport();
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
    auto t = MakeTransport();
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

TEST(SequencerTransportTest, TempoIsClampedToOneBpmMinimum) {
    auto t = MakeTransport();
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 0, 0));
    EXPECT_DOUBLE_EQ(t.TempoBpm(), 1.0) << "tempo_bpm_x100 = 0 must clamp, not stop time";
}

// SPP is measured in sixteenths, independently of the Pattern scale.
TEST(SequencerTransportTest, InternalContinueLocatesRequestedPosition) {
    auto t = MakeTransport();
    t.pattern().length = 4;
    t.pattern().scale = StepScale::Quarter;
    t.pattern().tracks[0].steps[0].on = true;
    t.pattern().tracks[0].steps[2].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));
    RunTicks(t, 300);  // partway into the pattern
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 0));
    ASSERT_FALSE(t.IsPlaying());

    t.ApplyTransport(SeqTransportMessage(
        SEQ_TRANSPORT_CONTINUE, SEQ_CLOCK_INTERNAL, SEQ_INPUT_PLAY, 0, 12000, 8));
    EXPECT_TRUE(t.IsPlaying());
    auto events = RunTicks(t, 10);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0].step, 2) << "SPP 8 is quarter-note step 2";
    EXPECT_EQ(events[0].frame, 0u) << "and the downbeat fires immediately";
}

// CONTINUE in MIDI mode arms (like PLAY) and starts on the master's
// MIDI CONTINUE, not by itself.
TEST(SequencerTransportTest, MidiContinueWaitsForFirstClockAtSpp) {
    auto t = MakeTransport();
    t.pattern().length = 1;
    t.pattern().scale = StepScale::Quarter;
    t.pattern().tracks[0].steps[0].on = true;

    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_CONTINUE, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0));
    EXPECT_TRUE(t.IsArmed());
    EXPECT_FALSE(t.IsPlaying());

    // MIDI CONTINUE with SPP 8 (sixteenths) = 8 * 24 internal ticks.
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_SPP, 0, 0, 0, 8));
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_CONTINUE, 0, 0, 0, 0));
    EXPECT_FALSE(t.IsPlaying());
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_TICK, 0, 1, 0, 0));
    EXPECT_TRUE(t.IsPlaying());
    EXPECT_FALSE(t.IsArmed());
    EXPECT_DOUBLE_EQ(t.follower().PhaseTicks(), 8.0 * 24.0);
}

// A standalone SPP message repositions the follower without touching the
// scheduler's armed/playing state.
TEST(SequencerTransportTest, SppRepositionsFollowerPhase) {
    auto t = MakeTransport();
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_STOP, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0));
    ASSERT_FALSE(t.IsPlaying());

    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_SPP, 0, 0, 0, /*spp_beats16=*/32));
    EXPECT_DOUBLE_EQ(t.follower().PhaseTicks(), 32.0 * 24.0);
    EXPECT_FALSE(t.IsPlaying()) << "SPP alone must not start the scheduler";
}

// MIDI START while NOT armed (no PLAY from the UI) must not start playback -
// the user's transport intent gates the master's.
TEST(SequencerTransportTest, UnarmedMidiStartDoesNotStartScheduler) {
    auto t = MakeTransport();
    t.pattern().tracks[0].steps[0].on = true;
    t.ApplyTransport(
        SeqTransportMessage(SEQ_TRANSPORT_STOP, SEQ_CLOCK_MIDI, SEQ_INPUT_PLAY, 0, 12000, 0));

    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_START, 0, 0, 0, 0));
    EXPECT_FALSE(t.IsPlaying());
}

// ---- Playhead feedback ----

TEST(SequencerTransportTest, PlayheadReportsStepAndBpm) {
    auto t = MakeTransport();
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

TEST(SequencerTransportTest, MidiPlayWaitsForClockAfterStart) {
    auto t = MakeTransport();
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

    // MIDI START prepares playback; the next clock is the downbeat.
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_START, 0, 0, 0, 0));
    EXPECT_FALSE(t.IsPlaying());
    t.OnMidiClock(MidiClockEventMessage(MIDI_CLK_TICK, 0, 1, 0, 0));
    EXPECT_TRUE(t.IsPlaying());
    EXPECT_FALSE(t.IsArmed());
}

TEST(SequencerTransportTest, MidiClockLocksAndTracksTempo) {
    auto t = MakeTransport();
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
    auto t = MakeTransport();
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

// ---- Mode fields ----

TEST(SequencerTransportTest, InputModeStoredFromTransport) {
    auto t = MakeTransport();
    t.ApplyTransport(SeqTransportMessage(
        SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0));
    EXPECT_EQ(t.InputMode(), SEQ_INPUT_LIVE_RECORD);
}

TEST(SequencerTransportTest, InternalClockHas24PpqnAndStopWinsOverBacklog) {
    for (uint16_t bpm: {2000, 12000, 30000}) {
        auto t = MakeTransport();
        t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, 0, 0, bpm, 0});
        unsigned ticks = 0, starts = 0;
        TriggerEvent events[32];
        SeqClockOutMessage out;
        for (unsigned ms = 0; ms < 60000; ++ms) {
            t.Tick(events, 32);
            while (t.PopClockOut(out)) {
                ticks += out.event == MIDI_CLK_TICK;
                starts += out.event == MIDI_CLK_START;
            }
        }
        EXPECT_EQ(starts, 1u);
        EXPECT_NEAR(ticks, bpm * 24.0 / 100, 1.0);
        RunTicks(t, 10000);  // foreground blocked: clocks fill the ring
        EXPECT_GT(t.ClockOutputDrops(), 0u);
        t.ApplyTransport({SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, 0, 0, bpm, 0});
        t.Tick(events, 32);
        ASSERT_TRUE(t.PopClockOut(out));
        EXPECT_EQ(out.event, MIDI_CLK_STOP);
        EXPECT_FALSE(t.PopClockOut(out));
    }
}

TEST(SequencerTransportTest, ContinuePublishesSppBeforeContinueAndClock) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_CONTINUE, SEQ_CLOCK_INTERNAL, 0, 0, 12000, 16383});
    RunTicks(t, 1);
    SeqClockOutMessage out;
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_SPP);
    EXPECT_EQ(out.spp_beats16, 16383);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_CONTINUE);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_TICK);
    EXPECT_FALSE(t.PopClockOut(out));
}

TEST(SequencerTransportTest, MidiSourceIsPinnedAndProjectStopDisarms) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, 0, 0, 12000, 0});
    t.OnMidiClock({MIDI_CLK_START, 1, 0, 0, 0});
    t.OnMidiClock({MIDI_CLK_TICK, 0, 1, 20833, 0});
    EXPECT_FALSE(t.IsPlaying());
    t.OnMidiClock({MIDI_CLK_TICK, 1, 1, 0, 0});
    EXPECT_TRUE(t.IsPlaying());
    t.OnMidiClock({MIDI_CLK_STOP, 0, 1, 0, 0});
    EXPECT_TRUE(t.IsPlaying());
    RunTicks(t, 50);
    SeqClockOutMessage out;
    EXPECT_FALSE(t.PopClockOut(out));  // never echo the master
    t.StopForProject();
    t.OnMidiClock({MIDI_CLK_START, 1, 1, 0, 0});
    t.OnMidiClock({MIDI_CLK_TICK, 1, 2, 0, 0});
    EXPECT_FALSE(t.IsPlaying());
}

TEST(SequencerTransportTest, SppContinueSkipsHistoryAndPreservesLocateAcrossDuplicateStop) {
    auto t = MakeTransport();
    t.pattern().length = 16;
    for (auto& step: t.pattern().tracks[0].steps)
        step.on = true;
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, 0, 0, 12000, 0});
    t.OnMidiClock({MIDI_CLK_SPP, 0, 0, 0, 16383});
    t.OnMidiClock({MIDI_CLK_STOP, 0, 0, 0, 0});
    t.OnMidiClock({MIDI_CLK_CONTINUE, 0, 0, 0, 0});
    EXPECT_TRUE(RunTicks(t, 10).empty());
    t.OnMidiClock({MIDI_CLK_TICK, 0, 1, 0, 0});
    const auto notes = RunTicks(t, 1);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].step, 15);
    EXPECT_EQ(notes[0].frame, 0u);
    EXPECT_EQ(t.scheduler().PlayheadLoop(), 1023u);
}

TEST(SequencerTransportTest, ClockSequenceGapDoesNotDivideAdjacentInterval) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, 0, 0, 12000, 0});
    t.OnMidiClock({MIDI_CLK_START, 0, 0, 0, 0});
    FeedMidiClocks(t, 24, 120);
    RunTicks(t, 42);
    t.OnMidiClock({MIDI_CLK_TICK, 0, 25, 20833, 0});
    EXPECT_NEAR(t.follower().MeasuredBpm(), 120, 0.1);
    const auto phase = t.follower().PhaseErrorTicks();
    t.OnMidiClock({MIDI_CLK_TICK, 0, 25, 10000, 0});
    EXPECT_DOUBLE_EQ(t.follower().PhaseErrorTicks(), phase);
}

TEST(SequencerTransportTest, FailedTransportRetriesLatestStateAndContinuePair) {
    auto t = MakeTransport();
    SeqClockOutMessage out;
    t.ApplyTransport({SEQ_TRANSPORT_CONTINUE, SEQ_CLOCK_INTERNAL, 0, 0, 12000, 99});
    RunTicks(t, 1);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_SPP);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_CONTINUE);
    t.ClockOutFailed(out);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_SPP);
    EXPECT_EQ(out.spp_beats16, 99);
    t.ClockOutFailed(out);
    t.ApplyTransport({SEQ_TRANSPORT_STOP, SEQ_CLOCK_INTERNAL, 0, 0, 12000, 0});
    RunTicks(t, 1);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_STOP);
    t.ClockOutFailed(out);
    ASSERT_TRUE(t.PopClockOut(out));
    EXPECT_EQ(out.event, MIDI_CLK_STOP);
    EXPECT_FALSE(t.PopClockOut(out));
}

TEST(SequencerTransportTest, MidiStartPreservesEarlyFirstStepAtDownbeat) {
    auto t = MakeTransport();
    t.pattern().tracks[0].steps[0].on = true;
    t.pattern().tracks[0].steps[0].micro_offset = -6;
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, 0, 0, 12000, 0});
    t.OnMidiClock({MIDI_CLK_START, 0, 0, 0, 0});
    t.OnMidiClock({MIDI_CLK_TICK, 0, 1, 0, 0});
    const auto events = RunTicks(t, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].step, 0);
    EXPECT_EQ(events[0].frame, 0u);
}

TEST(SequencerTransportTest, ExtremeInputTempoSaturatesWireReadback) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_MIDI, 0, 0, 12000, 0});
    t.OnMidiClock({MIDI_CLK_START, 0, 0, 0, 0});
    for (uint16_t i = 1; i <= 10; ++i) {
        RunTicks(t, 4);
        t.OnMidiClock({MIDI_CLK_TICK, 0, i, 3800, 0});
    }
    EXPECT_EQ(t.BuildPlayhead().measured_bpm_x100, 65535);
}

TEST(SequencerTransportTest, StepRecordAdvancesAfterChordAndReplacementKeepsReleaseIdentity) {
    auto t = MakeTransport();
    t.ApplyTransport(
        {SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_INTERNAL, SEQ_INPUT_STEP_RECORD, 1, 12000, 0});
    for (uint8_t i = 0; i < 5; ++i)
        t.RecordInput(0, static_cast<uint8_t>(60 + i), 1, 100, 1);
    EXPECT_TRUE(t.pattern().tracks[0].melodic);
    EXPECT_EQ(t.pattern().tracks[0].steps[0].notes[0].note, 64);
    for (uint8_t i = 0; i < 5; ++i)
        t.RecordInput(0, static_cast<uint8_t>(60 + i), 1, 0, 0);
    t.RecordInput(0, 70, 1, 100, 1);
    EXPECT_EQ(t.pattern().tracks[0].steps[1].notes[0].note, 70);
    SeqNotesMessage reply;
    t.BuildNotes({1, 0, 0, 0}, reply);
    EXPECT_TRUE(IsValidSeqNotes(reply));
    EXPECT_EQ(reply.notes[0].note, 64);
}
TEST(SequencerTransportTest, LiveRecordQuantizationDurationAndSongSafeEpochs) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0});
    RunTicks(t, 100);  // 19.2 ticks -> step 1 on nearest-step capture
    t.RecordInput(0, 0, 1, 127, 1);
    RunTicks(t, 250);
    t.RecordInput(0, 0, 1, 0, 0);
    const auto& step = t.pattern().tracks[0].steps[1];
    EXPECT_EQ(step.notes[0].note, 0);
    EXPECT_EQ(step.notes[0].velocity, 127);
    EXPECT_EQ(step.notes[0].gate_ticks, 48);
    EXPECT_EQ(step.micro_offset, 0);
    t.ApplyTransport(
        {SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 0, 12000, 0});
    t.RecordInput(0, 72, 1, 110, 1);  // 67.2 ticks -> step 2 + 19 ticks
    EXPECT_EQ(t.pattern().tracks[0].steps[2].micro_offset, 19);
    auto replacement = std::make_unique<WaveX::Sequencer::Pattern>();
    t.ReplacePattern(*replacement);
    t.RecordInput(0, 72, 1, 0, 0);
    EXPECT_EQ(t.pattern().tracks[0].steps[2].notes[0].velocity, 0);
}
TEST(SequencerTransportTest, LiveEraseOnlyMatchingPitchOnArmedTrack) {
    auto t = MakeTransport();
    for (uint8_t track = 0; track < 2; ++track) {
        auto& row = t.pattern().tracks[track];
        row.melodic = true;
        row.steps[0].on = true;
        row.steps[0].notes[0] = {60, 100, 24};
        row.steps[0].notes[1] = {64, 100, 24};
    }
    t.ApplyPatternOp({SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 16, 0});
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_ERASE, 1, 12000, 0});
    t.RecordInput(0, 60, 1, 100, 1);
    auto events = RunTicks(t, 1);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(t.pattern().tracks[0].steps[0].notes[0].velocity, 0);
    EXPECT_EQ(t.pattern().tracks[1].steps[0].notes[0].velocity, 100);
}

TEST(SequencerTransportTest, ManualLaneReplacementInvalidatesHeldCaptureGate) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0});
    t.RecordInput(0, 60, 1, 100, 1);
    RunTicks(t, 250);
    t.ApplyPatternOp({SEQ_OP_SET_NOTE_LANE, 0, 0, 0, 72 | (110 << 8), 96});
    t.RecordInput(0, 60, 1, 0, 0);
    const auto& lane = t.pattern().tracks[0].steps[0].notes[0];
    EXPECT_EQ(lane.note, 72);
    EXPECT_EQ(lane.velocity, 110);
    EXPECT_EQ(lane.gate_ticks, 96);
}

TEST(SequencerTransportTest, HalfStepRecordUsesSharedOffsetAndPreservesReleaseDuration) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 2, 12000, 0});
    RunTicks(t, 62);  // 11.904 ticks -> half of a 24-tick step
    t.RecordInput(0, 60, 1, 100, 1);
    EXPECT_EQ(t.pattern().tracks[0].steps[0].micro_offset, 12);
    RunTicks(t, 250);
    t.RecordInput(0, 60, 1, 0, 0);
    EXPECT_EQ(t.pattern().tracks[0].steps[0].notes[0].gate_ticks, 48);
    SeqNotesMessage reply;
    t.BuildNotes({1, 0, 0, 0}, reply);
    EXPECT_EQ(reply.quantize, 2);
    EXPECT_TRUE(IsValidSeqNotes(reply));
}

TEST(SequencerTransportTest, LiveLocksCaptureContainingStepWithoutChangingNotes) {
    auto t = MakeTransport();
    t.ApplyPatternOp({SEQ_OP_RECORD_TARGET, 2, 7, 0, 0, 0});
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0});
    RunTicks(t, 100);  // 19.2 ticks: note quantization would choose step 1.
    const auto epoch = t.PatternEpoch();
    EXPECT_TRUE(t.RecordControl({PARAM_FILTER_CUTOFF, 2, 12000}, epoch));
    const auto& step = t.pattern().tracks[2].steps[0];
    EXPECT_EQ(step.param_locks[0].param_id, PARAM_FILTER_CUTOFF);
    EXPECT_EQ(step.param_locks[0].value, 12000);
    EXPECT_FALSE(step.on);  // motion alone never creates notes or changes row mode
    EXPECT_FALSE(t.pattern().tracks[2].melodic);
    const auto revision = t.PatternRevision();
    EXPECT_FALSE(t.RecordControl({PARAM_FILTER_CUTOFF, 2, 12000}, epoch));
    EXPECT_EQ(t.PatternRevision(), revision);
    EXPECT_TRUE(t.RecordControl({PARAM_FILTER_CUTOFF, 2, 13000}, epoch));
    EXPECT_EQ(step.param_locks[0].value, 13000);
    EXPECT_EQ(step.param_locks[1].param_id, 0);
    RunTicks(t, 40);
    EXPECT_TRUE(t.RecordControl({PARAM_PAN, 2, 44000}, epoch));
    EXPECT_EQ(t.pattern().tracks[2].steps[1].param_locks[0].param_id, PARAM_PAN);
    // Wrap around a short Pattern; this is relative Pattern time, not song time.
    t.ApplyPatternOp({SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 2, 0});
    RunTicks(t, 125);
    EXPECT_TRUE(t.RecordControl({PARAM_PAN, 2, 23000}, epoch));
    EXPECT_EQ(t.pattern().tracks[2].steps[0].param_locks[1].value, 23000);
}

TEST(SequencerTransportTest, LiveLocksRejectInactiveWrongTrackAndStalePattern) {
    auto t = MakeTransport();
    const ControlChangeMessage control{PARAM_PAN, 0, 12345};
    EXPECT_FALSE(t.RecordControl(control, t.PatternEpoch()));
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_STEP_RECORD, 1, 12000, 0});
    EXPECT_FALSE(t.RecordControl(control, t.PatternEpoch()));
    t.ApplyTransport(
        {SEQ_TRANSPORT_CONFIGURE, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0});
    EXPECT_FALSE(t.RecordControl({PARAM_PAN, 1, 12345}, t.PatternEpoch()));
    EXPECT_FALSE(t.RecordControl({PARAM_VOLUME, 0, 12345}, t.PatternEpoch()));
    EXPECT_FALSE(t.RecordControl(control, t.PatternEpoch() + 1));
    EXPECT_TRUE(t.RecordControl(control, t.PatternEpoch()));
    const auto epoch = t.PatternEpoch();
    auto replacement = std::make_unique<WaveX::Sequencer::Pattern>();
    t.ReplacePattern(*replacement);
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 1, 12000, 0});
    EXPECT_FALSE(t.RecordControl(control, epoch));
    t.StopForProject();
    EXPECT_FALSE(t.RecordControl(control, t.PatternEpoch()));
    EXPECT_EQ(t.pattern().tracks[0].steps[0].param_locks[0].param_id, 0);
}

TEST(SequencerTransportTest, LiveLocksReplaceOldestSlotAndRemainIsolatedFromSong) {
    auto t = MakeTransport();
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 0, 12000, 0});
    for (uint8_t param = PARAM_FILTER_CUTOFF; param <= PARAM_ENVELOPE_SUSTAIN; ++param)
        EXPECT_TRUE(t.RecordControl({param, 0, uint16_t(param * 100)}, t.PatternEpoch()));
    const auto& step = t.pattern().tracks[0].steps[0];
    EXPECT_EQ(step.param_locks[0].param_id, PARAM_FILTER_RESONANCE);
    EXPECT_EQ(step.param_locks[3].param_id, PARAM_ENVELOPE_SUSTAIN);
    t.StopForProject();
    auto project = std::make_unique<WaveX::Sequencer::Project>();
    project->patterns[0].used = true;
    project->songs[0].used = true;
    project->songs[0].length = 1;
    project->songs[0].entries[0] = {0, 1};
    ASSERT_TRUE(t.StartSong(project.get(), 0, 0, true));
    EXPECT_FALSE(t.RecordControl({PARAM_PAN, 0, 12345}, t.PatternEpoch()));
    EXPECT_EQ(project->patterns[0].pattern.tracks[0].steps[0].param_locks[0].param_id, 0);
}

TEST(SequencerTransportTest, EvictionNoticeIdentifiesOldestAndIgnoresUpdates) {
    auto t = MakeTransport();
    for (uint8_t param: {PARAM_FILTER_CUTOFF, PARAM_FILTER_RESONANCE, PARAM_PAN, PARAM_GAIN})
        t.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK, 2, 3, param, 100, 0});
    EXPECT_EQ(t.LockNotice().count, 0u);
    t.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK, 2, 3, PARAM_PITCH, 200, 0});
    EXPECT_EQ(t.LockNotice().count, 1u);
    EXPECT_EQ(t.LockNotice().removed, PARAM_FILTER_CUTOFF);
    EXPECT_EQ(t.LockNotice().added, PARAM_PITCH);
    EXPECT_EQ(t.LockNotice().track, 2);
    EXPECT_EQ(t.LockNotice().step, 3);
    t.ApplyPatternOp({SEQ_OP_SET_PARAM_LOCK, 2, 3, PARAM_PITCH, 300, 0});
    EXPECT_EQ(t.LockNotice().count, 1u);
}

TEST(SequencerTransportTest, LiveLockPublicationReachesPlaybackAndComposesWithOtherEdits) {
    auto t = MakeTransport();
    t.ApplyPatternOp({SEQ_OP_PATTERN_LENGTH, 0, 0, 0, 2, 0});
    t.ApplyPatternOp({SEQ_OP_SET_STEP, 0, 0, 1, 100, 0});
    t.ApplyTransport({SEQ_TRANSPORT_PLAY, SEQ_CLOCK_INTERNAL, SEQ_INPUT_LIVE_RECORD, 0, 12000, 0});
    RunTicks(t, 10);
    ASSERT_TRUE(t.RecordControl({PARAM_PAN, 0, 12345}, t.PatternEpoch()));
    auto events = RunTicks(t, 250);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].param_lock_count, 1);
    EXPECT_EQ(events[0].param_locks[0].value, 12345);
    ASSERT_TRUE(t.RecordControl({PARAM_PAN, 0, 23456}, t.PatternEpoch()));
    t.ApplyPatternOp({SEQ_OP_SET_STEP_NOTE, 0, 0, 70, 0, 0});
    events = RunTicks(t, 250);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].note, 70);
    EXPECT_EQ(events[0].param_locks[0].value, 23456);
}
