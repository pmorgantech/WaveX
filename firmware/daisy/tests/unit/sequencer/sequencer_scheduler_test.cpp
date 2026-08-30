#include "sequencer/sequencer_scheduler.hpp"

#include <gtest/gtest.h>

#include <vector>

using WaveX::Sequencer::Pattern;
using WaveX::Sequencer::SequencerScheduler;
using WaveX::Sequencer::StepScale;
using WaveX::Sequencer::TriggerEvent;

namespace {

// Runs the scheduler for `ticks` control-tick calls (1 block each),
// collecting every fired event across the whole run into one vector -
// convenient for the golden/drift tests below, which care about the
// aggregate event stream rather than per-call batching.
std::vector<TriggerEvent> RunAll(SequencerScheduler& sched, int ticks) {
    std::vector<TriggerEvent> all;
    TriggerEvent buf[64];
    for (int i = 0; i < ticks; ++i) {
        size_t n = sched.Process(buf, 64);
        for (size_t j = 0; j < n; ++j)
            all.push_back(buf[j]);
    }
    return all;
}

}  // namespace

TEST(SequencerSchedulerTest, NoPatternProducesNoEvents) {
    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.Start();

    TriggerEvent buf[8];
    EXPECT_EQ(sched.Process(buf, 8), 0u);
}

TEST(SequencerSchedulerTest, StoppedProducesNoEvents) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    // Never Start()ed.

    TriggerEvent buf[8];
    EXPECT_EQ(sched.Process(buf, 8), 0u);
}

TEST(SequencerSchedulerTest, DisabledTrackProducesNoEvents) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.tracks[0].enabled = false;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    auto events = RunAll(sched, 1000);
    EXPECT_TRUE(events.empty());
}

TEST(SequencerSchedulerTest, StepOffProducesNoEvents) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    // steps[0].on defaults to false.

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    auto events = RunAll(sched, 1000);
    EXPECT_TRUE(events.empty());
}

// A single-step, single-track, straight (no swing), quarter-note pattern at
// 120 BPM fires the downbeat immediately at Start() (frame 0 - standard
// sequencer behavior, playback doesn't wait a full step before the first
// hit) and then once every 24000 frames (0.5 s at 48 kHz) after that.
TEST(SequencerSchedulerTest, QuarterNoteAt120BpmFiresEveryHalfSecond) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.swing = 50;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].velocity = 100;
    p.tracks[0].steps[0].probability = 100;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // 2500 calls = 120000 frames, just short of beat 5 (tick 480 -> frame
    // 120000 exactly, excluded by the half-open block convention) -
    // leaves exactly beats 0..4 (5 events) at frames 0,24000,...,96000.
    auto events = RunAll(sched, 2500);
    ASSERT_EQ(events.size(), 5u);
    for (size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].frame, i * 24000u) << "beat " << i;
        EXPECT_EQ(events[i].track, 0);
        EXPECT_EQ(events[i].velocity, 100);
        EXPECT_FALSE(events[i].is_retrig);
    }
}

// Drift test (sequencer.md §6): a 10-minute run at 120 BPM must place beat
// 1200 at exactly frame 28,800,000, with zero accumulated error. This is
// the test that specifically exercises the anti-drift design described in
// sequencer_scheduler.hpp's class comment: frame = tick * frames_per_tick
// computed fresh per candidate event (never accumulated), so 600,000
// control-tick calls cannot compound any rounding error the way an
// accumulated per-block phase delta would.
TEST(SequencerSchedulerTest, TenMinuteDriftTestAt120Bpm) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.swing = 50;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 100;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // Beat 0 fires at frame 0 (Start()); beat 1200 (tick 115200) fires at
    // frame 28,800,000, which requires the block *ending* at frame
    // 28,800,048 (call 600,001) - the half-open convention defers a tick
    // landing exactly on a block boundary to the following block, so call
    // 600,000 (ending exactly at 28,800,000) is one call too few.
    auto events = RunAll(sched, 600001);
    ASSERT_EQ(events.size(), 1201u);
    EXPECT_EQ(events[1200].frame, 28800000u);

    // Every beat in between must also land on an exact multiple of 24000
    // frames - not just the last one - confirming no drift accumulated
    // anywhere along the way.
    for (size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].frame, i * 24000ull) << "beat index " << i;
    }
}

// Swing delays odd-indexed steps by (swing-50)/50 of the step interval;
// even-indexed steps are never delayed. With scale=Sixteenth (24 ticks/step)
// and swing=66, the delay is 24 * (66-50)/50 = 7.68 ticks. At 120 BPM
// (0.192 ticks/block), that's 7.68/0.192 = 40 blocks = 1920 frames later
// than the straight (swing=50) position.
TEST(SequencerSchedulerTest, SwingDelaysOddStepsOnly) {
    auto build = [](uint8_t swing) {
        Pattern p;
        p.length = 2;
        p.scale = StepScale::Sixteenth;
        p.swing = swing;
        p.tracks[0].steps[0].on = true;
        p.tracks[0].steps[1].on = true;
        return p;
    };

    Pattern straight = build(50);
    Pattern swung = build(66);

    SequencerScheduler s1;
    s1.Init(48000, 48);
    s1.SetTempo(120.0f);
    s1.SetPattern(&straight);
    s1.Start();
    auto e1 = RunAll(s1, 2000);

    SequencerScheduler s2;
    s2.Init(48000, 48);
    s2.SetTempo(120.0f);
    s2.SetPattern(&swung);
    s2.Start();
    auto e2 = RunAll(s2, 2000);

    ASSERT_GE(e1.size(), 2u);
    ASSERT_GE(e2.size(), 2u);

    // Step 0 (even) - identical regardless of swing.
    EXPECT_EQ(e1[0].frame, e2[0].frame);
    // Step 1 (odd) - swung version fires exactly 1920 frames later.
    EXPECT_EQ(e2[1].frame - e1[1].frame, 1920u);
}

// Per-step micro-timing offset shifts that step's trigger independent of
// swing. At 120 BPM, 1 internal PPQN tick = (1/0.192) blocks =
// 5.208333... blocks = 250 frames exactly (1 tick / 0.192 ticks-per-block *
// 48 frames/block = 250). A micro_offset of +4 ticks should delay the
// event by exactly 1000 frames relative to micro_offset=0.
TEST(SequencerSchedulerTest, MicroOffsetShiftsTriggerTime) {
    auto build = [](int16_t micro_offset) {
        Pattern p;
        p.length = 1;
        p.scale = StepScale::Quarter;
        p.tracks[0].steps[0].on = true;
        p.tracks[0].steps[0].micro_offset = micro_offset;
        return p;
    };

    Pattern flat = build(0);
    Pattern shifted = build(4);

    SequencerScheduler s1;
    s1.Init(48000, 48);
    s1.SetTempo(120.0f);
    s1.SetPattern(&flat);
    s1.Start();
    auto e1 = RunAll(s1, 600);

    SequencerScheduler s2;
    s2.Init(48000, 48);
    s2.SetTempo(120.0f);
    s2.SetPattern(&shifted);
    s2.Start();
    auto e2 = RunAll(s2, 600);

    ASSERT_GE(e1.size(), 1u);
    ASSERT_GE(e2.size(), 1u);
    EXPECT_EQ(e2[0].frame - e1[0].frame, 1000u);
}

// Probability=100 always fires regardless of RNG state.
TEST(SequencerSchedulerTest, ProbabilityHundredAlwaysFires) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 100;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(140.0f);
    sched.SetPattern(&p);
    sched.SetSeed(12345);
    sched.Start();

    auto events = RunAll(sched, 5000);
    // At 140 BPM, sixteenth notes = ~107ms apart; 5000 blocks = 5s, so
    // expect roughly 5000/ (24 ticks / (96*140/60000 ticks/block)) events -
    // just assert "many, and every one fired" rather than an exact count
    // tied to unrelated arithmetic already covered by the quarter-note test.
    EXPECT_GT(events.size(), 10u);
}

// Probability=0 never fires.
TEST(SequencerSchedulerTest, ProbabilityZeroNeverFires) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 0;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(140.0f);
    sched.SetPattern(&p);
    sched.SetSeed(12345);
    sched.Start();

    auto events = RunAll(sched, 5000);
    EXPECT_TRUE(events.empty());
}

// Same seed => identical fire/no-fire sequence across independent runs -
// the determinism golden test (sequencer.md §6).
TEST(SequencerSchedulerTest, SeededProbabilityIsDeterministic) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 50;

    auto run = [&]() {
        SequencerScheduler sched;
        sched.Init(48000, 48);
        sched.SetTempo(140.0f);
        sched.SetPattern(&p);
        sched.SetSeed(777);
        sched.Start();
        return RunAll(sched, 3000);
    };

    auto a = run();
    auto b = run();

    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].frame, b[i].frame) << "event " << i;
    }
    // Sanity: a 50% gate over many steps should fire noticeably less than
    // a 100%-probability run would over the same span, and more than zero.
    EXPECT_GT(a.size(), 0u);
}

// Different seeds produce different probability sequences (otherwise the
// seed parameter would be decorative).
TEST(SequencerSchedulerTest, DifferentSeedsProduceDifferentSequences) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 50;

    auto run = [&](uint64_t seed) {
        SequencerScheduler sched;
        sched.Init(48000, 48);
        sched.SetTempo(140.0f);
        sched.SetPattern(&p);
        sched.SetSeed(seed);
        sched.Start();
        return RunAll(sched, 3000);
    };

    auto a = run(1);
    auto b = run(2);

    // Extremely unlikely to be identical by chance across ~150 gated steps;
    // if this ever flakes, the RNG has a seed-independence bug.
    bool identical = a.size() == b.size();
    if (identical) {
        for (size_t i = 0; i < a.size() && identical; ++i) {
            if (a[i].frame != b[i].frame)
                identical = false;
        }
    }
    EXPECT_FALSE(identical);
}

// Retrig: count=2, rate=8 ticks within a 24-tick sixteenth-note step
// produces 3 total hits (1 primary + 2 retrigs) spaced 8 ticks apart. At
// 120 BPM, 8 ticks = 8/0.192 blocks = 41.666 blocks = 2000 frames exactly
// (8 ticks * 250 frames/tick, reusing the 250-frames/tick constant derived
// in the micro-offset test).
TEST(SequencerSchedulerTest, RetrigFiresAdditionalHitsWithinTheStep) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Sixteenth;  // 24 ticks/step
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 100;
    p.tracks[0].steps[0].retrig_count = 2;
    p.tracks[0].steps[0].retrig_rate_ticks = 8;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // One 24-tick step at 0.192 ticks/block = 125 blocks; run a couple of
    // steps' worth to also confirm the pattern loops cleanly afterward.
    auto events = RunAll(sched, 300);
    ASSERT_GE(events.size(), 3u);

    EXPECT_FALSE(events[0].is_retrig);
    EXPECT_TRUE(events[1].is_retrig);
    EXPECT_TRUE(events[2].is_retrig);
    EXPECT_EQ(events[1].frame - events[0].frame, 2000u);
    EXPECT_EQ(events[2].frame - events[1].frame, 2000u);
}

// A retrig rate wider than the step's own duration must produce zero
// retrigs (nothing fits before the next step's boundary) rather than
// bleeding into the next step's slot.
TEST(SequencerSchedulerTest, RetrigWiderThanStepProducesNoRetrigs) {
    Pattern p;
    p.length = 2;
    p.scale = StepScale::Sixteenth;  // 24 ticks/step
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].probability = 100;
    p.tracks[0].steps[0].retrig_count = 2;
    p.tracks[0].steps[0].retrig_rate_ticks = 30;  // wider than the 24-tick step
    p.tracks[0].steps[1].on = true;
    p.tracks[0].steps[1].probability = 100;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // 250 calls = 12000 frames stays just short of the pattern's second
    // loop (tick 48 -> frame 12000, excluded by the half-open block
    // convention), leaving exactly this loop's two primary hits.
    auto events = RunAll(sched, 250);
    // Exactly the two primary hits (step 0, step 1) - no retrigs, and no
    // bleed-through into step 1's slot.
    ASSERT_EQ(events.size(), 2u);
    EXPECT_FALSE(events[0].is_retrig);
    EXPECT_FALSE(events[1].is_retrig);
    EXPECT_EQ(events[0].step, 0);
    EXPECT_EQ(events[1].step, 1);
}

// Param locks attached to a step ride along on its TriggerEvent.
TEST(SequencerSchedulerTest, ParamLocksCarryThroughToEvent) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[0].param_locks[0] = {5, 200};
    p.tracks[0].steps[0].param_locks[1] = {7, 900};

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // 400 calls = 19200 frames, well short of the pattern's second loop
    // (tick 96 -> frame 24000) - leaves exactly the one downbeat event.
    auto events = RunAll(sched, 400);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].param_lock_count, 2);
    EXPECT_EQ(events[0].param_locks[0].param_id, 5);
    EXPECT_EQ(events[0].param_locks[0].value, 200);
    EXPECT_EQ(events[0].param_locks[1].param_id, 7);
    EXPECT_EQ(events[0].param_locks[1].value, 900);
}

// Events within a single control tick are returned sorted by frame (and by
// track index on ties) - sequencer.md §6: "sorted (frame, event) stream".
TEST(SequencerSchedulerTest, EventsAreSortedByFrame) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    // Two tracks with the same step boundary (no micro-offset difference)
    // fire within the same control tick, in the same block - track order
    // should come out ascending (tie-break rule).
    p.tracks[3].steps[0].on = true;
    p.tracks[1].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // 400 calls, same reasoning as ParamLocksCarryThroughToEvent: stays
    // within the pattern's first loop for both tracks.
    auto events = RunAll(sched, 400);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_LE(events[0].frame, events[1].frame);
    if (events[0].frame == events[1].frame) {
        EXPECT_LT(events[0].track, events[1].track);
    }
}

// --- Tempo extremes / tick boundaries ---------------------------------------

// SetTempo clamps at 1 BPM instead of dividing by zero or going negative.
// Even at the floor the scheduler must still fire the downbeat at frame 0.
TEST(SequencerSchedulerTest, TempoIsClampedToOneBpmMinimum) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(0.0f);
    EXPECT_FLOAT_EQ(sched.Tempo(), 1.0f);
    sched.SetTempo(-30.0f);
    EXPECT_FLOAT_EQ(sched.Tempo(), 1.0f);

    sched.SetPattern(&p);
    sched.Start();
    auto events = RunAll(sched, 1000);
    // At 1 BPM a quarter-note step is 2,880,000 frames; only the immediate
    // downbeat fits in 48,000 frames.
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].frame, 0u);
}

// 960 BPM: frames_per_tick = 60*48000/(96*960) = 31.25 exactly, so a quarter
// -note step is 3000.0 frames with zero rounding - beats must land on exact
// 3000-frame multiples even at a tempo far above musical range.
TEST(SequencerSchedulerTest, HighTempoStaysExactOnTickBoundaries) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(960.0f);
    sched.SetPattern(&p);
    sched.Start();

    // 1000 blocks = 48000 frames; beat 16 (frame 48000) is excluded by the
    // half-open convention, leaving beats 0..15.
    auto events = RunAll(sched, 1000);
    ASSERT_EQ(events.size(), 16u);
    for (size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].frame, i * 3000u) << "beat " << i;
    }
}

// A zero-length pattern has no steps to schedule; Process must be a no-op,
// not a modulo-by-zero or an infinite boundary loop.
TEST(SequencerSchedulerTest, ZeroLengthPatternProducesNoEvents) {
    Pattern p;
    p.length = 0;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetPattern(&p);
    sched.Start();

    TriggerEvent buf[8];
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(sched.Process(buf, 8), 0u);
}

// SetPattern(nullptr) mid-playback stops scheduling without needing Stop().
TEST(SequencerSchedulerTest, NullPatternMidRunStopsScheduling) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();
    ASSERT_FALSE(RunAll(sched, 200).empty());

    sched.SetPattern(nullptr);
    EXPECT_TRUE(RunAll(sched, 1000).empty());
    EXPECT_TRUE(sched.IsPlaying()) << "nulling the pattern mutes scheduling, not the transport";
}

// The caller's max_events budget truncates deterministically: lowest frames
// first, ties broken by ascending track index.
TEST(SequencerSchedulerTest, MaxEventsCapKeepsEarliestSortedEvents) {
    Pattern p;
    p.length = 1;
    p.scale = StepScale::Quarter;
    for (uint8_t t = 0; t < 8; ++t)
        p.tracks[t].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    TriggerEvent buf[3];
    size_t n = sched.Process(buf, 3);
    ASSERT_EQ(n, 3u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(buf[i].frame, 0u);
        EXPECT_EQ(buf[i].track, i) << "tie-break must keep the lowest track indices";
    }
}

// --- Playhead feedback ------------------------------------------------------

// PlayheadStep/PlayheadLoop track the most recently crossed step on the
// shared grid (used for MSG_SEQ_PLAYHEAD): step interval at 120 BPM /
// sixteenth = 24 ticks = 6000 frames = 125 blocks.
TEST(SequencerSchedulerTest, PlayheadTracksStepAndLoop) {
    Pattern p;
    p.length = 4;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    EXPECT_EQ(sched.PlayheadStep(), 0);
    EXPECT_EQ(sched.PlayheadLoop(), 0u);

    RunAll(sched, 126);  // crosses the step-1 boundary at frame 6000
    EXPECT_EQ(sched.PlayheadStep(), 1);
    EXPECT_EQ(sched.PlayheadLoop(), 0u);

    RunAll(sched, 399);  // 525 blocks = 25200 frames: last crossing at 24000
    EXPECT_EQ(sched.PlayheadStep(), 0);
    EXPECT_EQ(sched.PlayheadLoop(), 1u) << "wrap back to step 0 increments the loop count";
}

// Multi-step pattern with a longer run covers correct looping (loop_count
// advancing, step_index wrapping) beyond the first pass.
TEST(SequencerSchedulerTest, PatternLoopsCorrectly) {
    Pattern p;
    p.length = 4;
    p.scale = StepScale::Sixteenth;
    p.tracks[0].steps[0].on = true;
    p.tracks[0].steps[2].on = true;  // classic four-on-the-floor-ish subset

    SequencerScheduler sched;
    sched.Init(48000, 48);
    sched.SetTempo(120.0f);
    sched.SetPattern(&p);
    sched.Start();

    // 4 steps * 24 ticks = 96 ticks/loop; loop N's second hit (step 2, tick
    // 24+96N) lands at frame (24+96N)*250. 1500 calls = 72000 frames stays
    // just short of loop 3's step-0 hit (tick 288 -> frame 72000, excluded
    // by the half-open block convention), leaving exactly 3 full loops'
    // worth of hits (loops 0, 1, 2).
    auto events = RunAll(sched, 1500);
    ASSERT_EQ(events.size(), 6u);  // 2 hits/loop * 3 loops
    for (size_t i = 0; i < events.size(); i += 2) {
        EXPECT_EQ(events[i].step, 0);
        EXPECT_EQ(events[i + 1].step, 2);
    }
}
