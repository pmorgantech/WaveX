#pragma once

// Sample-accurate step scheduler (roadmap Phase 2 item 1; design:
// docs/features/sequencer.md §1-2). HAL-free: operates on a caller-owned
// Pattern pointer and plain arithmetic, so it is host-testable without any
// Daisy hardware or cross compiler - same pattern as voice_manager.hpp.
//
// Placement (per sequencer.md §1): this class is meant to be driven once
// per 1 kHz control tick (one audio block) from the Daisy audio callback,
// same call site as the paraphonic envelope / CV staging. Not yet wired
// there - that is a later "engine wiring" stage once the pattern-edit
// protocol and double-buffer discipline (sequencer.md §4) exist. This
// stage is the engine core plus its host test suite only.
//
// Timing model / anti-drift design (sequencer.md §2 wants "fixed point...
// to avoid drift" - the design below achieves that goal, but not via a
// literal Q32.32 accumulator; see the note below for why):
//
// The absolute audio frame counter (`frame_counter_`) is the ONLY quantity
// ever accumulated across Process() calls, and it accumulates by adding
// the exact integer `block_size_` each time - pure integer arithmetic,
// provably driftless regardless of tempo.
//
// Every pattern-derived quantity (a step's internal-PPQN-tick position) is
// computed fresh from pure integer inputs (step index, loop count) each
// time it's needed - never accumulated - so it carries no history to drift.
//
// The one place a fractional/irrational-in-binary quantity appears is the
// tick<->frame conversion (tempo in BPM rarely gives a dyadic frames-per-tick
// ratio). The key design choice: convert tick -> frame with a single fresh
// multiply against the *exact* target tick value every time a candidate
// event is checked (TickToFrame()), rather than accumulating a per-block
// tick delta and comparing against a running phase. An earlier version of
// this class did the latter (an accumulated Q32.32 phase compared against
// a fuzzy per-block boundary) and it failed the 10-minute drift test at
// exactly 120 BPM: the per-block delta for 0.192 ticks/block isn't exactly
// representable in binary, so repeatedly adding its rounded fixed-point
// value 600,000 times accumulated enough error to flip an exact-integer
// target tick onto the wrong side of a block boundary, landing beat 1200
// at frame 28,799,999 instead of 28,800,000. Computing frame = tick *
// frames_per_tick fresh (no accumulation) avoids that entirely - for a
// "nice" tempo/rate pair like 120 BPM @ 48 kHz, frames_per_tick is exactly
// 250.0 in double precision, so target_tick * 250.0 is an *exact* result
// for any integer target_tick, with zero rounding at all.
//
// Real-time-safety: Process() does no allocation and no I/O; the local
// working buffer is a fixed-size stack array. Safe to call from the audio
// callback's control-tick context (architecture.md §1) PROVIDED the
// Pattern this scheduler points at is not mutated concurrently - that
// discipline (double-buffered pattern rows, edits applied between steps)
// is the caller's responsibility per sequencer.md §4 and is not
// implemented in this class.

#include "pattern.hpp"
#include <algorithm>
#include <cstdint>

namespace WaveX {
namespace Sequencer {

// Upper bound on events returned by a single Process() call. Sized for the
// realistic worst case (every track's step boundary plus a couple of
// retrigs landing in the same 1 ms control tick) rather than the
// structural worst case (16 tracks x 9 hits = 144), which would require
// step intervals on the order of a control tick - not a musically
// reachable configuration. Events beyond this bound in one call are
// dropped (documented, not UB); callers needing more headroom can raise
// this constant.
static constexpr size_t kMaxEventsPerTick = 32;

class SequencerScheduler {
   public:
    void Init(uint32_t sample_rate, uint16_t block_size) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        block_size_ = block_size > 0 ? block_size : 48;
        RecomputeTempoConstants();
    }

    void SetTempo(float bpm) {
        bpm_ = bpm > 1.0f ? bpm : 1.0f;
        RecomputeTempoConstants();
    }

    float Tempo() const { return bpm_; }

    // Not owned; must stay valid (and, if playing, must only be mutated
    // under the caller's double-buffer discipline - see class comment)
    // for as long as this scheduler points at it. Passing nullptr stops
    // scheduling (Process() becomes a no-op) without needing a separate
    // Stop() call.
    void SetPattern(const Pattern* pattern) {
        pattern_ = pattern;
        if (playing_ && pattern_) {
            for (uint8_t t = 0; t < kMaxTracks; ++t) {
                ClampAndRescheduleTrack(t);
            }
        }
    }

    // Deterministic probability-gate sequence (sequencer.md §6: "golden
    // tests for... probability (seeded RNG)"). A seed of 0 is remapped to
    // a fixed nonzero value - xorshift's state must never be zero.
    void SetSeed(uint64_t seed) { seed_ = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL; }

    // Resets phase/frame/step positions to zero and reseeds the RNG from
    // the last SetSeed() value. Pause/resume/song-position semantics
    // (MIDI CONTINUE/SPP) belong to the transport layer
    // (docs/features/midi-sync-tempo-follower.md §4), not this core. The
    // very first step of every track fires immediately (tick 0), matching
    // standard sequencer/drum-machine behavior: the downbeat sounds the
    // instant playback starts, it does not wait one full step interval.
    void Start() {
        frame_counter_ = 0;
        rng_state_ = seed_;
        playing_ = true;
        playhead_step_ = 0;
        playhead_loop_ = 0;
        for (uint8_t t = 0; t < kMaxTracks; ++t) {
            track_state_[t] = TrackState{};
            if (pattern_)
                RescheduleTrack(t, 0, 0);
        }
    }

    void Stop() { playing_ = false; }

    bool IsPlaying() const { return playing_; }

    // Playhead position on the shared pattern grid: the step index most
    // recently crossed (i.e. currently sounding), and how many full pattern
    // loops have elapsed since Start(). All tracks advance in lockstep on the
    // shared grid (micro-offset/retrig shift frames within a step, not the
    // step index), so track 0's crossings define the global playhead. Meant
    // for coalesced UI feedback (SeqPlayheadMessage), not sample-accurate use.
    uint8_t PlayheadStep() const { return playhead_step_; }
    uint32_t PlayheadLoop() const { return playhead_loop_; }

    uint64_t CurrentFrame() const { return frame_counter_; }

    // Advances by exactly one control tick (one audio block). Appends any
    // triggers that fall within this tick to `out_events` (capacity
    // `max_events`), sorted by frame (ties broken by track index for
    // determinism), and returns the count written. No-op (returns 0)
    // if stopped or no pattern is set.
    size_t Process(TriggerEvent* out_events, size_t max_events) {
        if (!playing_ || !pattern_ || pattern_->length == 0)
            return 0;

        const uint64_t block_start_frame = frame_counter_;
        const uint64_t block_end_frame = block_start_frame + block_size_;  // exclusive

        TriggerEvent local[kMaxEventsPerTick];
        size_t local_count = 0;

        for (uint8_t t = 0; t < kMaxTracks; ++t) {
            TrackState& ts = track_state_[t];
            const Track& track = pattern_->tracks[t];

            // Bounded iteration guard: structurally a track could need
            // multiple boundary crossings in one tick (see class comment);
            // cap so a pathological config can never spin unbounded.
            for (int guard = 0; guard < 8; ++guard) {
                // Retrigs pending from a step already primary-fired.
                while (ts.pending_retrigs > 0) {
                    if (ts.next_retrig_tick >= ts.retrig_clip_tick) {
                        // Would land at or past the next step's own
                        // boundary - drop the rest rather than overlap it.
                        ts.pending_retrigs = 0;
                        break;
                    }
                    const uint64_t rframe = TickToFrame(ts.next_retrig_tick);
                    if (rframe >= block_end_frame)
                        break;  // not due yet this block; stays pending
                    if (track.enabled && local_count < kMaxEventsPerTick) {
                        local[local_count] =
                            MakeEvent(t,
                                      ts.step_index,
                                      ts.pending_retrig_velocity,
                                      true,
                                      nullptr,
                                      0,
                                      ClampFrame(rframe, block_start_frame, block_end_frame));
                        ++local_count;
                    }
                    ts.next_retrig_tick += ts.retrig_rate_ticks;
                    --ts.pending_retrigs;
                }

                const uint64_t tframe = TickToFrame(ts.next_trigger_tick);
                if (tframe >= block_end_frame)
                    break;

                // Primary step boundary reached.
                const Step& step = track.steps[ts.step_index];
                // Track 0's grid crossings define the global playhead (all
                // tracks share the grid and cross together). Capture the step
                // being fired here, before advancing to the next one.
                if (t == 0) {
                    playhead_step_ = ts.step_index;
                    playhead_loop_ = ts.loop_count;
                }
                const uint8_t len = PatternLength();
                uint8_t next_step = static_cast<uint8_t>(ts.step_index + 1);
                uint32_t next_loop = ts.loop_count;
                if (next_step >= len) {
                    next_step = 0;
                    ++next_loop;
                }
                const double upcoming_tick = ComputeTriggerTick(t, next_step, next_loop);

                bool scheduled_retrig = false;
                if (track.enabled && step.on) {
                    if (NextRandomPercent() < step.probability) {
                        if (local_count < kMaxEventsPerTick) {
                            local[local_count] =
                                MakeEvent(t,
                                          ts.step_index,
                                          step.velocity,
                                          false,
                                          step.param_locks,
                                          CountLocks(step),
                                          ClampFrame(tframe, block_start_frame, block_end_frame));
                            ++local_count;
                        }
                        if (step.retrig_count > 0 && step.retrig_rate_ticks > 0) {
                            const double rate = static_cast<double>(step.retrig_rate_ticks);
                            const double first_retrig = ts.next_trigger_tick + rate;
                            // Only schedule if at least the first retrig
                            // fits before the next step's own boundary - a
                            // rate wider than the step's duration means
                            // nothing would ever fire anyway.
                            if (first_retrig < upcoming_tick) {
                                ts.pending_retrigs = step.retrig_count;
                                ts.retrig_rate_ticks = rate;
                                ts.next_retrig_tick = first_retrig;
                                ts.retrig_clip_tick = upcoming_tick;
                                ts.pending_retrig_velocity = step.velocity;
                                scheduled_retrig = true;
                            }
                        }
                    }
                }
                // Any retrig state left over from a previous step (already
                // clipped, never fully drained) must not survive into this
                // step's slot - clear it unless we just scheduled fresh ones.
                if (!scheduled_retrig)
                    ts.pending_retrigs = 0;

                ts.step_index = next_step;
                ts.loop_count = next_loop;
                ts.next_trigger_tick = upcoming_tick;
            }
        }

        std::sort(local, local + local_count, [](const TriggerEvent& a, const TriggerEvent& b) {
            if (a.frame != b.frame)
                return a.frame < b.frame;
            return a.track < b.track;
        });

        size_t n = std::min(local_count, max_events);
        for (size_t i = 0; i < n; ++i)
            out_events[i] = local[i];

        frame_counter_ = block_end_frame;
        return n;
    }

   private:
    struct TrackState {
        uint8_t step_index = 0;
        uint32_t loop_count = 0;
        double next_trigger_tick = 0.0;
        uint8_t pending_retrigs = 0;
        double next_retrig_tick = 0.0;
        double retrig_rate_ticks = 0.0;
        double retrig_clip_tick = 0.0;
        uint8_t pending_retrig_velocity = 0;
    };

    uint8_t PatternLength() const {
        if (!pattern_)
            return 1;
        uint8_t len = pattern_->length;
        if (len == 0)
            return 1;
        if (len > kMaxSteps)
            return kMaxSteps;
        return len;
    }

    void RecomputeTempoConstants() {
        // frames = tick * frames_per_tick. For "nice" bpm/sample-rate pairs
        // (e.g. 120 BPM @ 48 kHz => 250.0 exactly) this is an exact double,
        // and even when it isn't, computing it once here and reusing it in
        // a single multiply per candidate event (rather than a per-block
        // accumulation) is what keeps TickToFrame() drift-free - see the
        // class comment.
        frames_per_tick_ = 60.0 * static_cast<double>(sample_rate_) /
                           (static_cast<double>(kInternalPpqn) * static_cast<double>(bpm_));
    }

    uint64_t TickToFrame(double tick) const {
        double f = tick * frames_per_tick_;
        if (f <= 0.0)
            return 0;
        return static_cast<uint64_t>(f + 0.5);  // round to nearest frame
    }

    static uint64_t ClampFrame(uint64_t frame, uint64_t block_start, uint64_t block_end) {
        if (frame < block_start)
            return block_start;
        if (frame >= block_end)
            return block_end - 1;
        return frame;
    }

    // Swing offset (ticks) for a step index, shared across all tracks
    // (swing is pattern-level). Even-indexed steps ("on the beat") are
    // never delayed; odd-indexed steps ("the off-beat") are delayed by a
    // fraction of the step interval proportional to how far `swing` is
    // above 50 (straight). Returned as a double (not truncated to an
    // integer tick count) - the delay is frequently a fractional number of
    // internal ticks (e.g. 24 * 0.32 = 7.68) and truncating it away was an
    // earlier bug in this class (lost ~15% of the intended swing amount).
    double SwingOffsetTicks(uint8_t step_index, uint16_t step_interval_ticks) const {
        if (!pattern_ || (step_index % 2) == 0)
            return 0.0;
        double frac = (static_cast<double>(pattern_->swing) - 50.0) / 50.0;  // 0..0.5 over 50..75
        return static_cast<double>(step_interval_ticks) * frac;
    }

    // Absolute internal-tick position of `step_index`'s pattern-level
    // boundary (scale + swing; shared across tracks), for the given loop
    // iteration. Computed fresh from the (step_index, loop_count) integer
    // inputs every time - never accumulated - so it carries no drift.
    double StepBoundaryTicks(uint8_t step_index, uint32_t loop_count) const {
        const uint16_t interval = StepIntervalTicks(pattern_->scale);
        const double pattern_length_ticks = static_cast<double>(PatternLength()) * interval;
        const double base = static_cast<double>(loop_count) * pattern_length_ticks +
                            static_cast<double>(step_index) * interval;
        return base + SwingOffsetTicks(step_index, interval);
    }

    // Adds the per-(track,step) micro-timing offset on top of the shared
    // scale+swing boundary.
    double ComputeTriggerTick(uint8_t track_index, uint8_t step_index, uint32_t loop_count) const {
        double boundary = StepBoundaryTicks(step_index, loop_count);
        const Step& step = pattern_->tracks[track_index].steps[step_index];
        return boundary + static_cast<double>(step.micro_offset);
    }

    void RescheduleTrack(uint8_t track_index, uint8_t step_index, uint32_t loop_count) {
        TrackState& ts = track_state_[track_index];
        ts.step_index = step_index;
        ts.loop_count = loop_count;
        ts.next_trigger_tick = ComputeTriggerTick(track_index, step_index, loop_count);
        ts.pending_retrigs = 0;
    }

    // Used by SetPattern() when a new pattern is swapped in mid-playback:
    // clamps the track's current step index into the new pattern's length
    // and recomputes its next boundary, preserving loop position
    // best-effort. See sequencer.md §4 for the intended discipline
    // (swaps happen at step boundaries in practice); this is a defensive
    // fallback, not a precision guarantee across an in-flight swap.
    void ClampAndRescheduleTrack(uint8_t track_index) {
        TrackState& ts = track_state_[track_index];
        const uint8_t len = PatternLength();
        if (ts.step_index >= len)
            ts.step_index = 0;
        ts.next_trigger_tick = ComputeTriggerTick(track_index, ts.step_index, ts.loop_count);
        ts.pending_retrigs = 0;
    }

    static uint8_t CountLocks(const Step& step) {
        uint8_t n = 0;
        for (const auto& lock: step.param_locks) {
            if (lock.param_id != 0)
                ++n;
            else
                break;
        }
        return n;
    }

    static TriggerEvent MakeEvent(uint8_t track,
                                  uint8_t step,
                                  uint8_t velocity,
                                  bool is_retrig,
                                  const ParamLock* locks,
                                  uint8_t lock_count,
                                  uint64_t frame) {
        TriggerEvent ev;
        ev.track = track;
        ev.step = step;
        ev.velocity = velocity;
        ev.is_retrig = is_retrig;
        ev.frame = frame;
        ev.param_lock_count = lock_count;
        for (uint8_t i = 0; i < lock_count && i < kMaxParamLocks; ++i)
            ev.param_locks[i] = locks[i];
        return ev;
    }

    // xorshift64* - fast, deterministic, and reproducible across runs
    // given the same seed (sequencer.md §6 golden-test requirement).
    uint8_t NextRandomPercent() {
        rng_state_ ^= rng_state_ >> 12;
        rng_state_ ^= rng_state_ << 25;
        rng_state_ ^= rng_state_ >> 27;
        uint64_t result = rng_state_ * 0x2545F4914F6CDD1DULL;
        return static_cast<uint8_t>((result >> 56) % 100);
    }

    const Pattern* pattern_ = nullptr;
    uint32_t sample_rate_ = 48000;
    uint16_t block_size_ = 48;
    float bpm_ = 120.0f;
    double frames_per_tick_ = 250.0;
    uint64_t frame_counter_ = 0;
    bool playing_ = false;
    uint64_t seed_ = 0x9E3779B97F4A7C15ULL;
    uint64_t rng_state_ = 0x9E3779B97F4A7C15ULL;
    uint8_t playhead_step_ = 0;
    uint32_t playhead_loop_ = 0;
    TrackState track_state_[kMaxTracks];
};

}  // namespace Sequencer
}  // namespace WaveX
