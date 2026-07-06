#include "sequencer/tempo_follower.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <random>

using WaveX::Sequencer::SyncLockState;
using WaveX::Sequencer::TempoFollower;

namespace {

// Drives a simulated MIDI clock stream into `tf`, modeling the two
// independent clock domains the design doc (midi-sync-tempo-follower.md
// §2) cares about:
//   - `true_time_us` is a drift-free reference we use to judge results,
//     advanced by `period_us_at(i)` each of the `count` intervals.
//   - `arrival_jitter_us_at(i)` perturbs when the clock byte "reaches" the
//     Daisy (affects how many Tick() calls run first - the domain
//     TempoFollower's phase servo actually observes).
//   - `esp_jitter_us_at(i)` perturbs the *reported delta* (models jitter
//     already baked into the ESP32-side timestamp before it's forwarded).
// Per the class's call contract, OnMidiClock() is not called for interval
// index 0 relative to a fresh start (there is no previous byte) - the
// harness starts the simulated delta stream from the first interval and
// simply treats interval 0 as "the delta between byte 0 and byte 1".
//
// IMPORTANT: `true_time_us`/`last_observed_us`/`tick_budget_us` are the
// harness's own running clock state and must persist across the entire
// simulated stream. A caller that wants to observe state between events
// (flap detection, error sampling) must pass `after_each` rather than
// calling this function repeatedly with count=1 in a loop - each call is
// independent and resets these accumulators, which silently truncates the
// fractional tick remainder (up to ~1ms) on every single call instead of
// carrying it forward, compounding into a large artificial timing error
// over many calls. This bit the first version of the jitter and ppm-drift
// tests below.
void SimulateClocks(
    TempoFollower& tf,
    int count,
    const std::function<double(int)>& period_us_at,
    const std::function<double(int)>& arrival_jitter_us_at = [](int) { return 0.0; },
    const std::function<double(int)>& esp_jitter_us_at = [](int) { return 0.0; },
    const std::function<void(int)>& after_each = [](int) {}) {
    double true_time_us = 0.0;
    double last_observed_us = 0.0;
    double tick_budget_us = 0.0;

    for (int i = 0; i < count; ++i) {
        true_time_us += period_us_at(i);
        double observed_us = true_time_us + arrival_jitter_us_at(i);
        double elapsed = observed_us - last_observed_us;
        if (elapsed < 0.0)
            elapsed = 0.0;
        tick_budget_us += elapsed;
        int ticks = static_cast<int>(tick_budget_us / 1000.0);
        tick_budget_us -= ticks * 1000.0;
        for (int t = 0; t < ticks; ++t)
            tf.Tick();

        double delta = period_us_at(i) + esp_jitter_us_at(i);
        if (delta < 0.0)
            delta = 0.0;
        tf.OnMidiClock(static_cast<uint32_t>(delta + 0.5));
        last_observed_us = observed_us;
        after_each(i);
    }
}

double PeriodUsForBpm(double bpm) {
    return 2.5e6 / bpm;
}

}  // namespace

// --- Transport / basic state -------------------------------------------

TEST(TempoFollowerTest, DefaultsToUnlockedFreeRunningAtNominal) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(100.0f);
    EXPECT_EQ(tf.State(), SyncLockState::Unlocked);
    EXPECT_FLOAT_EQ(tf.MeasuredBpm(), 100.0f);
}

TEST(TempoFollowerTest, StopHaltsPhaseAdvance) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.OnStart();
    for (int i = 0; i < 100; ++i)
        tf.Tick();
    double phase_before = tf.PhaseTicks();
    EXPECT_GT(phase_before, 0.0);

    tf.OnStop();
    for (int i = 0; i < 100; ++i)
        tf.Tick();
    EXPECT_DOUBLE_EQ(tf.PhaseTicks(), phase_before);
}

TEST(TempoFollowerTest, StartResetsPhaseToZero) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.OnStart();
    for (int i = 0; i < 500; ++i)
        tf.Tick();
    EXPECT_GT(tf.PhaseTicks(), 0.0);

    tf.OnStart();  // re-start mid-stream
    EXPECT_DOUBLE_EQ(tf.PhaseTicks(), 0.0);
}

// Continue+SPP golden math: SPP is in MIDI beats (16th notes); internal
// ticks per 16th note = 96/4 = 24.
TEST(TempoFollowerTest, ContinueSetsPhaseFromSongPositionPointer) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.OnContinue(16);  // 16 sixteenth-notes in = bar 2 of a 4/4 pattern
    EXPECT_DOUBLE_EQ(tf.PhaseTicks(), 16.0 * 24.0);
    EXPECT_TRUE(tf.IsPlaying());
}

TEST(TempoFollowerTest, DisablingMidiSyncFallsBackToNominalBpm) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);
    SimulateClocks(tf, 10, [](int) { return PeriodUsForBpm(140.0); });
    ASSERT_NEAR(tf.MeasuredBpm(), 140.0f, 0.5f);

    tf.SetSyncSource(false);
    EXPECT_EQ(tf.State(), SyncLockState::Unlocked);
    EXPECT_FLOAT_EQ(tf.MeasuredBpm(), 120.0f);
}

// --- Clean lock ----------------------------------------------------------

TEST(TempoFollowerTest, CleanLockWithinFiveClocksAt120Bpm) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);

    SimulateClocks(tf, 5, [](int) { return PeriodUsForBpm(120.0); });

    EXPECT_EQ(tf.State(), SyncLockState::Locked);
    EXPECT_NEAR(tf.MeasuredBpm(), 120.0f, 0.1f);
}

TEST(TempoFollowerTest, CleanLockAtVariousTempos) {
    for (double bpm: {60.0, 90.0, 140.0, 180.0}) {
        TempoFollower tf;
        tf.Init(48000, 48);
        tf.SetNominalBpm(120.0f);
        tf.SetSyncSource(true);
        SimulateClocks(tf, 8, [bpm](int) { return PeriodUsForBpm(bpm); });
        EXPECT_EQ(tf.State(), SyncLockState::Locked) << "bpm=" << bpm;
        EXPECT_NEAR(tf.MeasuredBpm(), bpm, 0.5) << "bpm=" << bpm;
    }
}

// --- Jitter ---------------------------------------------------------------

// +-2ms arrival jitter (Daisy domain) plus +-200us delta jitter (ESP
// domain) must not prevent a lock, must not cause state flapping, and must
// keep the phase-error RMS bounded to a small fraction of an internal tick
// over a long run.
TEST(TempoFollowerTest, JitterStaysLockedWithBoundedPhaseError) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);
    tf.OnStart();  // phase only advances in Tick() while playing

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> arrival_jitter(-2000.0, 2000.0);
    std::uniform_real_distribution<double> esp_jitter(-200.0, 200.0);

    // Lock on a clean prefix first (jitter during acquisition could
    // legitimately delay lock - that's not what this test is checking).
    SimulateClocks(tf, 6, [](int) { return PeriodUsForBpm(120.0); });
    ASSERT_EQ(tf.State(), SyncLockState::Locked);

    int flap_count = 0;
    double sum_sq_error = 0.0;
    int samples = 0;
    SyncLockState last_state = tf.State();
    const int kEvents = 2000;
    SimulateClocks(
        tf,
        kEvents,
        [](int) { return PeriodUsForBpm(120.0); },
        [&](int) { return arrival_jitter(rng); },
        [&](int) { return esp_jitter(rng); },
        [&](int) {
            if (tf.State() != last_state) {
                ++flap_count;
                last_state = tf.State();
            }
            if (tf.State() == SyncLockState::Locked) {
                double e = tf.PhaseErrorTicks();
                sum_sq_error += e * e;
                ++samples;
            }
        });

    EXPECT_EQ(tf.State(), SyncLockState::Locked);
    EXPECT_LE(flap_count, 1);  // allow at most one transient state change
    ASSERT_GT(samples, 0);
    double rms = std::sqrt(sum_sq_error / samples);
    EXPECT_LT(rms, 0.5) << "phase error RMS too high: " << rms << " internal ticks";
}

// --- Drift (ppm offset) ---------------------------------------------------

// A "master" running 50 ppm fast must not cause unbounded beat drift - the
// proportional rate trim should settle to a small, bounded steady-state
// phase error (not zero, but not growing) even over a simulated hour.
TEST(TempoFollowerTest, FiftyPpmOffsetProducesNoUnboundedDrift) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);
    tf.OnStart();  // phase only advances in Tick() while playing

    const double kPpmFast = 50.0e-6;
    auto period_fn = [kPpmFast](int) { return PeriodUsForBpm(120.0) * (1.0 - kPpmFast); };

    // Lock first.
    SimulateClocks(tf, 6, period_fn);
    ASSERT_EQ(tf.State(), SyncLockState::Locked);

    // One simulated hour at 120 BPM = 48 clocks/sec * 3600 s = 172800 clocks,
    // run as a single continuous stream (see SimulateClocks' class comment
    // on why chunking this into repeated calls would silently truncate the
    // fractional tick remainder every chunk boundary).
    double max_error_seen = std::fabs(tf.PhaseErrorTicks());
    const int kTotalClocks = 172800;
    SimulateClocks(
        tf,
        kTotalClocks,
        period_fn,
        [](int) { return 0.0; },
        [](int) { return 0.0; },
        [&](int) {
            double e = std::fabs(tf.PhaseErrorTicks());
            if (e > max_error_seen)
                max_error_seen = e;
        });

    EXPECT_EQ(tf.State(), SyncLockState::Locked);
    // Bounded, not growing without limit: well under a full internal tick
    // sustained over the whole hour.
    EXPECT_LT(max_error_seen, 1.0) << "steady-state phase error grew unbounded";
}

// --- Tempo jump ------------------------------------------------------------

TEST(TempoFollowerTest, StepTempoJumpReacquiresWithinOneBar) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);

    SimulateClocks(tf, 8, [](int) { return PeriodUsForBpm(120.0); });
    ASSERT_EQ(tf.State(), SyncLockState::Locked);
    ASSERT_NEAR(tf.MeasuredBpm(), 120.0, 0.5);

    // Step to 140 BPM. One bar of 4/4 at 24 PPQN = 96 clocks.
    SimulateClocks(tf, 96, [](int) { return PeriodUsForBpm(140.0); });

    EXPECT_NEAR(tf.MeasuredBpm(), 140.0, 1.0);
    // Should have re-locked (not stuck Acquiring/Freewheel) by the end of
    // the bar.
    EXPECT_EQ(tf.State(), SyncLockState::Locked);
}

TEST(TempoFollowerTest, GradualTempoRampTracksWithinOneBpm) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);

    SimulateClocks(tf, 8, [](int) { return PeriodUsForBpm(120.0); });
    ASSERT_EQ(tf.State(), SyncLockState::Locked);

    // Ramp 120 -> 140 over 8 bars (8*96 = 768 clocks), linear in BPM, as one
    // continuous stream (see SimulateClocks' class comment on why chunking
    // this into per-event calls would silently corrupt the harness's own
    // timing bookkeeping, even though this particular test doesn't happen
    // to read anything sensitive to it - BPM comes straight from the
    // per-event delta, not from accumulated harness state).
    const int kClocks = 768;
    SimulateClocks(tf, kClocks, [kClocks](int i) {
        double frac = static_cast<double>(i) / static_cast<double>(kClocks);
        double bpm = 120.0 + frac * 20.0;
        return PeriodUsForBpm(bpm);
    });

    EXPECT_NEAR(tf.MeasuredBpm(), 140.0, 1.0);
}

// --- Dropout / freewheel ---------------------------------------------------

TEST(TempoFollowerTest, DropoutEntersFreewheelAndKeepsPhaseMonotonic) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);
    tf.OnStart();

    SimulateClocks(tf, 8, [](int) { return PeriodUsForBpm(120.0); });
    ASSERT_EQ(tf.State(), SyncLockState::Locked);

    double phase_before_gap = tf.PhaseTicks();

    // 300 ms gap with no clocks - well over 2x the ~20.8ms period.
    for (int i = 0; i < 300; ++i)
        tf.Tick();

    EXPECT_EQ(tf.State(), SyncLockState::Freewheel);
    EXPECT_GT(tf.PhaseTicks(), phase_before_gap);  // kept advancing, didn't stall

    // Clock resumes.
    SimulateClocks(tf, 8, [](int) { return PeriodUsForBpm(120.0); });
    EXPECT_EQ(tf.State(), SyncLockState::Locked);
}

TEST(TempoFollowerTest, ResumeAfterDropoutDoesNotStepPhase) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);
    tf.OnStart();

    SimulateClocks(tf, 8, [](int) { return PeriodUsForBpm(120.0); });
    ASSERT_EQ(tf.State(), SyncLockState::Locked);

    for (int i = 0; i < 300; ++i)
        tf.Tick();
    ASSERT_EQ(tf.State(), SyncLockState::Freewheel);

    double phase_just_before_resume = tf.PhaseTicks();
    tf.OnMidiClock(static_cast<uint32_t>(PeriodUsForBpm(120.0)));

    // Re-entering acquisition must not have stepped phase_ discontinuously
    // (OnMidiClock() in Acquiring/Freewheel-resume never writes phase_
    // directly, only period_us_/state - this pins that invariant).
    EXPECT_DOUBLE_EQ(tf.PhaseTicks(), phase_just_before_resume);
}

// --- Outlier rejection / hard re-acquire -----------------------------------

TEST(TempoFollowerTest, SingleGlitchDeltaDoesNotDeragLock) {
    TempoFollower tf;
    tf.Init(48000, 48);
    tf.SetNominalBpm(120.0f);
    tf.SetSyncSource(true);

    SimulateClocks(tf, 8, [](int) { return PeriodUsForBpm(120.0); });
    ASSERT_EQ(tf.State(), SyncLockState::Locked);
    double bpm_before = tf.MeasuredBpm();

    // One wildly out-of-range delta (way outside the 25% outlier band).
    tf.OnMidiClock(static_cast<uint32_t>(PeriodUsForBpm(120.0) * 3.0));
    EXPECT_EQ(tf.State(), SyncLockState::Locked);
    EXPECT_NEAR(tf.MeasuredBpm(), bpm_before, 0.5f);

    // Back to normal - still locked, tempo estimate unaffected by the blip.
    SimulateClocks(tf, 4, [](int) { return PeriodUsForBpm(120.0); });
    EXPECT_EQ(tf.State(), SyncLockState::Locked);
    EXPECT_NEAR(tf.MeasuredBpm(), 120.0f, 0.5f);
}
