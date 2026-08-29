#pragma once

// Sequencer transport controller (roadmap Phase 2; design bridge across
// docs/features/sequencer.md, midi-sync-tempo-follower.md). HAL-free glue
// that owns the two engine cores - SequencerScheduler (step timing) and
// TempoFollower (MIDI-clock PLL) - and translates the Phase 2 wire messages
// (protocol.h SEQ_* / MIDI_CLOCK) into edits and transport commands, then
// produces the per-tick TriggerEvent stream plus coalesced playhead
// feedback. Host-testable like the cores it wraps; nothing here touches
// daisy_seed.h or the audio callback (that wiring lives in audio_engine.cpp
// and consumes this class's Tick() output).
//
// Threading/placement (target): Tick() is driven once per 1 kHz control tick
// from the audio callback context; Apply*/OnMidi* are driven from the
// main-loop message dispatch. The Pattern this owns is single-buffered here;
// the double-buffer / edit-between-steps discipline (sequencer.md §4) is a
// later refinement - for now edits are plain field writes and a test/caller
// must not interleave an edit with a Tick() from another context. This class
// adds no locking (it has no HAL to lock with).
//
// Clock modes (midi-sync-tempo-follower.md §4):
//   INTERNAL: transport PLAY starts the scheduler immediately at the set
//             tempo; MIDI clock events are ignored.
//   MIDI:     transport PLAY arms; the scheduler actually starts when a MIDI
//             START (or the first clock in CONTINUE) arrives, so sequencer
//             step 0 aligns to the master's downbeat. Each tick the scheduler
//             tempo is set to the follower's servo-corrected instantaneous
//             BPM so it tracks the external clock's phase correction rather
//             than free-running at the raw estimate.

#include "spi_protocol/protocol.h"

#include "pattern.hpp"
#include "sequencer_scheduler.hpp"
#include "tempo_follower.hpp"
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Sequencer {

class SequencerTransport {
   public:
    void Init(uint32_t sample_rate, uint16_t block_size) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        block_size_ = block_size > 0 ? block_size : 48;
        scheduler_.Init(sample_rate_, block_size_);
        scheduler_.SetPattern(&pattern_);
        scheduler_.SetTempo(static_cast<float>(tempo_bpm_));
        follower_.Init(sample_rate_, block_size_);
        follower_.SetNominalBpm(static_cast<float>(tempo_bpm_));
    }

    // Direct pattern access for test setup and (later) UI-side mirroring.
    Pattern& pattern() { return pattern_; }
    const Pattern& pattern() const { return pattern_; }

    // ---- Transport + mode (MSG_SEQ_TRANSPORT) ----
    void ApplyTransport(const Protocol::SeqTransportMessage& m) {
        tempo_bpm_ = static_cast<double>(m.tempo_bpm_x100) / 100.0;
        if (tempo_bpm_ < 1.0)
            tempo_bpm_ = 1.0;
        scheduler_.SetTempo(static_cast<float>(tempo_bpm_));
        follower_.SetNominalBpm(static_cast<float>(tempo_bpm_));

        input_mode_ = m.input_mode;
        quantize_ = m.quantize;

        const bool want_midi = (m.clock_source == Protocol::SEQ_CLOCK_MIDI);
        follower_.SetSyncSource(want_midi);
        using_midi_ = want_midi;

        switch (m.command) {
            case Protocol::SEQ_TRANSPORT_STOP:
                scheduler_.Stop();
                follower_.OnStop();
                armed_ = false;
                break;
            case Protocol::SEQ_TRANSPORT_PLAY:
                if (using_midi_) {
                    // Arm: wait for MIDI START to align step 0 to the downbeat.
                    armed_ = true;
                    scheduler_.Stop();
                } else {
                    scheduler_.Start();
                    follower_.OnStart();
                    armed_ = false;
                }
                break;
            case Protocol::SEQ_TRANSPORT_CONTINUE:
                if (using_midi_) {
                    armed_ = true;
                    scheduler_.Stop();
                } else {
                    // No native "resume from arbitrary step" in the scheduler
                    // core yet; CONTINUE in internal mode restarts from the
                    // top (documented limitation - song position resume rides
                    // on the MIDI SPP path below when slaved).
                    scheduler_.Start();
                    follower_.OnContinue(m.song_position);
                    armed_ = false;
                }
                break;
            default:
                break;
        }
    }

    // ---- Pattern edits (MSG_SEQ_PATTERN_OP) ----
    // Bounds-checked; an out-of-range track/step or unknown op is a silent
    // no-op (the wire is untrusted; never index past the fixed arrays).
    void ApplyPatternOp(const Protocol::SeqPatternOpMessage& m) {
        using namespace Protocol;
        switch (m.op) {
            case SEQ_OP_SET_STEP:
                if (StepValid(m.track, m.step)) {
                    Step& s = pattern_.tracks[m.track].steps[m.step];
                    s.on = (m.arg_u8 != 0);
                    s.velocity = ClampVelocity(m.arg_u16);
                }
                break;
            case SEQ_OP_TOGGLE_STEP:
                if (StepValid(m.track, m.step)) {
                    Step& s = pattern_.tracks[m.track].steps[m.step];
                    s.on = !s.on;
                }
                break;
            case SEQ_OP_SET_STEP_PROB:
                if (StepValid(m.track, m.step)) {
                    pattern_.tracks[m.track].steps[m.step].probability =
                        m.arg_u8 > 100 ? 100 : m.arg_u8;
                }
                break;
            case SEQ_OP_SET_STEP_MICRO:
                if (StepValid(m.track, m.step)) {
                    Step& s = pattern_.tracks[m.track].steps[m.step];
                    s.retrig_count = m.arg_u8 > kMaxRetrigCount ? kMaxRetrigCount : m.arg_u8;
                    s.retrig_rate_ticks = static_cast<uint8_t>(m.arg_u16 & 0xFF);
                    s.micro_offset = m.arg_s16;
                }
                break;
            case SEQ_OP_TRACK_MUTE:
                if (m.track < kMaxTracks)
                    pattern_.tracks[m.track].enabled = (m.arg_u8 != 0);
                break;
            case SEQ_OP_PATTERN_LENGTH: {
                uint16_t len = m.arg_u16;
                if (len < 1)
                    len = 1;
                if (len > kMaxSteps)
                    len = kMaxSteps;
                pattern_.length = static_cast<uint8_t>(len);
                break;
            }
            case SEQ_OP_PATTERN_SCALE:
                if (m.arg_u8 <= static_cast<uint8_t>(StepScale::EighthTriplet))
                    pattern_.scale = static_cast<StepScale>(m.arg_u8);
                break;
            case SEQ_OP_PATTERN_SWING: {
                uint8_t sw = m.arg_u8;
                if (sw < 50)
                    sw = 50;
                if (sw > 75)
                    sw = 75;
                pattern_.swing = sw;
                break;
            }
            case SEQ_OP_SET_PARAM_LOCK:
                if (StepValid(m.track, m.step) && m.arg_u8 != 0)
                    SetParamLock(pattern_.tracks[m.track].steps[m.step], m.arg_u8, m.arg_u16);
                break;
            case SEQ_OP_CLEAR_PARAM_LOCKS:
                if (StepValid(m.track, m.step)) {
                    Step& s = pattern_.tracks[m.track].steps[m.step];
                    for (auto& lock: s.param_locks)
                        lock = ParamLock{};
                }
                break;
            default:
                break;
        }
    }

    // ---- MIDI clock (MSG_MIDI_CLOCK_EVENT) ----
    void OnMidiClock(const Protocol::MidiClockEventMessage& m) {
        using namespace Protocol;
        switch (m.event) {
            case MIDI_CLK_TICK:
                follower_.OnMidiClock(m.esp_delta_us);
                break;
            case MIDI_CLK_START:
                follower_.OnStart();
                if (using_midi_ && armed_) {
                    scheduler_.Start();
                    armed_ = false;
                }
                break;
            case MIDI_CLK_CONTINUE:
                follower_.OnContinue(m.spp_beats16);
                if (using_midi_ && armed_) {
                    scheduler_.Start();
                    armed_ = false;
                }
                break;
            case MIDI_CLK_STOP:
                follower_.OnStop();
                scheduler_.Stop();
                break;
            case MIDI_CLK_SPP:
                follower_.OnContinue(m.spp_beats16);
                break;
            default:
                break;
        }
    }

    // ---- MIDI CC (MSG_MIDI_CC) ----
    // Hook for the modulation layer (param-locks-and-modulation.md §6). No
    // mod matrix exists yet; record the latest value per CC so a future
    // consumer (and tests) can observe forwarding works end to end.
    void OnMidiCc(const Protocol::MidiCcMessage& m) {
        last_cc_ = m.cc;
        last_cc_value_ = m.value;
        last_cc_channel_ = m.channel;
        ++cc_count_;
    }

    // Advance exactly one control tick. In MIDI mode, first advances the
    // follower and syncs the scheduler tempo to its servo-corrected rate.
    // Returns trigger events for this block (see SequencerScheduler::Process).
    size_t Tick(TriggerEvent* out_events, size_t max_events) {
        if (using_midi_) {
            follower_.Tick();
            if (scheduler_.IsPlaying())
                scheduler_.SetTempo(static_cast<float>(follower_.InstantaneousBpm()));
        }
        return scheduler_.Process(out_events, max_events);
    }

    // Coalesced playhead snapshot for MSG_SEQ_PLAYHEAD. sync_state maps the
    // follower state to the wire encoding (0=internal,1=acquiring,2=locked,
    // 3=freewheel); in internal mode it is always 0.
    Protocol::SeqPlayheadMessage BuildPlayhead() const {
        uint8_t sync_state = 0;
        if (using_midi_) {
            switch (follower_.State()) {
                case SyncLockState::Acquiring:
                    sync_state = 1;
                    break;
                case SyncLockState::Locked:
                    sync_state = 2;
                    break;
                case SyncLockState::Freewheel:
                    sync_state = 3;
                    break;
                default:
                    sync_state = 0;
                    break;
            }
        }
        double bpm = using_midi_ ? follower_.MeasuredBpm() : tempo_bpm_;
        uint16_t bpm_x100 = static_cast<uint16_t>(bpm * 100.0 + 0.5);
        return Protocol::SeqPlayheadMessage(
            /*pattern=*/0,
            scheduler_.PlayheadStep(),
            scheduler_.IsPlaying() ? 1 : 0,
            sync_state,
            bpm_x100,
            scheduler_.PlayheadLoop());
    }

    bool IsPlaying() const { return scheduler_.IsPlaying(); }
    bool IsArmed() const { return armed_; }
    bool UsingMidiSync() const { return using_midi_; }
    SyncLockState SyncState() const { return follower_.State(); }
    double TempoBpm() const { return tempo_bpm_; }
    uint8_t InputMode() const { return input_mode_; }

    // MIDI-CC observation (for the not-yet-built mod layer / tests).
    uint32_t CcCount() const { return cc_count_; }
    uint8_t LastCc() const { return last_cc_; }
    uint8_t LastCcValue() const { return last_cc_value_; }
    uint8_t LastCcChannel() const { return last_cc_channel_; }

    // Direct core access for tests that want to assert on the underlying
    // scheduler/follower state.
    const SequencerScheduler& scheduler() const { return scheduler_; }
    const TempoFollower& follower() const { return follower_; }

   private:
    static bool StepValid(uint8_t track, uint8_t step) {
        return track < kMaxTracks && step < kMaxSteps;
    }
    static uint8_t ClampVelocity(uint16_t v) { return v > 127 ? 127 : static_cast<uint8_t>(v); }

    static void SetParamLock(Step& s, uint8_t param_id, uint16_t value) {
        // Overwrite an existing lock for this param, else fill the first free
        // slot. Full + no match => drop the oldest (slot 0 shift), matching
        // the "oldest evicted" rule in param-locks-and-modulation.md §2.
        for (auto& lock: s.param_locks) {
            if (lock.param_id == param_id) {
                lock.value = value;
                return;
            }
        }
        for (auto& lock: s.param_locks) {
            if (lock.param_id == 0) {
                lock.param_id = param_id;
                lock.value = value;
                return;
            }
        }
        for (uint8_t i = 1; i < kMaxParamLocks; ++i)
            s.param_locks[i - 1] = s.param_locks[i];
        s.param_locks[kMaxParamLocks - 1] = ParamLock{param_id, value};
    }

    Pattern pattern_;
    SequencerScheduler scheduler_;
    TempoFollower follower_;

    uint32_t sample_rate_ = 48000;
    uint16_t block_size_ = 48;
    double tempo_bpm_ = 120.0;
    bool using_midi_ = false;
    bool armed_ = false;  // MIDI mode: PLAY received, waiting for START
    uint8_t input_mode_ = 0;
    uint8_t quantize_ = 0;

    uint32_t cc_count_ = 0;
    uint8_t last_cc_ = 0;
    uint8_t last_cc_value_ = 0;
    uint8_t last_cc_channel_ = 0;
};

}  // namespace Sequencer
}  // namespace WaveX
