#pragma once
#include "sequencer/parameter_locks.hpp"

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
// Threading/placement: this object is callback-owned. The engine feeds it
// complete wire commands through an SPSC queue; Apply*/OnMidi* therefore run
// in the callback too. Pattern edits land in `pending_pattern_`, then swap to
// `active_pattern_` immediately after a shared step boundary. The scheduler
// can never read a Pattern that is being edited, and a current step never
// changes underneath its own trigger calculation.
//
// Clock modes (midi-sync-tempo-follower.md §4):
//   INTERNAL: transport PLAY starts the scheduler immediately at the set
//             tempo; MIDI clock events are ignored.
//   MIDI:     transport PLAY arms; the scheduler actually starts when a MIDI
//             START/CONTINUE is followed by its first CLOCK, so sequencer
//             step 0 aligns to the master's downbeat. Each tick the scheduler
//             tempo is set to the follower's servo-corrected instantaneous
//             BPM so it tracks the external clock's phase correction rather
//             than free-running at the raw estimate.

#include "memory_sections.h"
#include "spi_protocol/protocol.h"

#include "bss_static.hpp"
#include "midi/event_ring.hpp"
#include "note_recorder.hpp"
#include "pattern.hpp"
#include "sequencer/project_data.hpp"
#include "sequencer_scheduler.hpp"
#include "tempo_follower.hpp"
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Sequencer {

class SequencerTransport {
   public:
    SequencerTransport() = default;
    SequencerTransport(const SequencerTransport&) = delete;
    SequencerTransport& operator=(const SequencerTransport&) = delete;
    void Init(uint32_t sample_rate, uint16_t block_size) {
        recorder_.Target(0, 0);
        clock_out_.Init();
        output_control_.store(0);
        output_control_seen_ = 0;
        output_continue_pending_ = false;
        output_sequence_ = source_sequence_ = 0;
        using_midi_ = armed_ = false;
        clock_frame_ = 0;
        clock_frame_published_.store(0);
        source_ = -1;
        source_sequence_valid_ = false;
        midi_enabled_ = awaiting_clock_ = output_playing_ = false;
        position_ = 0;
        pending_output_ = 0;
        song_project_ = nullptr;
        pending_pattern_ = &pending_storage_;
        WaveX::ReconstructInPlace(*pending_pattern_);
        launch_buffer_ = nullptr;
        launch_cancelled_ = false;
        active_slot_ = 0;
        active_epoch_ = 1;
        active_pattern_ = *pending_pattern_;
        pending_pattern_dirty_ = false;
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        block_size_ = block_size > 0 ? block_size : 48;
        scheduler_.Init(sample_rate_, block_size_);
        scheduler_.SetPattern(&active_pattern_);
        scheduler_.SetTempo(static_cast<float>(tempo_bpm_));
        follower_.Init(sample_rate_, block_size_);
        follower_.SetNominalBpm(static_cast<float>(tempo_bpm_));
    }

    // Direct pattern access is for pre-play test setup only. Runtime edits use
    // ApplyPatternOp(), which marks the pending copy for the next safe swap.
    Pattern& pattern() { return *pending_pattern_; }
    const Pattern& pattern() const {
        return song_project_ ? song_project_->patterns[active_slot_].pattern : *pending_pattern_;
    }

    uint32_t PatternEpoch() const { return active_epoch_; }
    uint32_t PatternRevision() const { return pattern_revision_; }
    // Callback only. Persistence validates before handing this private buffer
    // over. Stop without changing tempo, sync configuration or Track bindings.
    void ReplacePattern(const Pattern& pattern) {
        StopSong();
        scheduler_.Stop();
        follower_.OnStop();
        armed_ = midi_enabled_ = awaiting_clock_ = false;
        *pending_pattern_ = pattern;
        pending_pattern_dirty_ = true;
        ++pattern_revision_;
        AdvanceEpoch();
    }

    // Callback-only Project boundary: stop without replacing session settings.
    void StopForProject() {
        recorder_.Reset();
        StopSong();
        scheduler_.Stop();
        follower_.OnStop();
        armed_ = midi_enabled_ = awaiting_clock_ = false;
    }
    Protocol::SeqTransportMessage SessionSettings() const {
        return {Protocol::SEQ_TRANSPORT_STOP,
                using_midi_ ? Protocol::SEQ_CLOCK_MIDI : Protocol::SEQ_CLOCK_INTERNAL,
                input_mode_,
                quantize_,
                static_cast<uint16_t>(tempo_bpm_ * 100.0 + 0.5),
                0};
    }

    // Callback only: exchange buffer ownership, never copy an outgoing Pattern
    // onto the ISR stack. After completion the foreground owns the old working
    // buffer, including edits accepted while the destination was queued.
    bool LaunchPattern(Pattern*& buffer, uint8_t slot) {
        if (armed_ || launch_buffer_ || song_project_)
            return false;
        launch_cancelled_ = false;
        launch_slot_ = slot;
        if (scheduler_.IsPlaying()) {
            if (!scheduler_.QueuePattern(buffer))
                return false;
            launch_buffer_ = &buffer;
        } else
            SwapWorkingPattern(buffer);
        return true;
    }
    bool LaunchPending() const { return launch_buffer_ != nullptr; }
    bool LaunchCancelled() const { return launch_cancelled_; }
    void SetPatternSlot(uint8_t slot) { active_slot_ = slot; }

    // The Project is frozen by the foreground owner until SongActive() is false
    // and its exchange publishes release. Only this callback reads it meanwhile.
    bool StartSong(const Project* project, uint8_t song, uint8_t entry, bool loop) {
        if (!project || song >= kMaxSongs || !project->songs[song].used ||
            project->songs[song].length > kMaxSongEntries || entry >= project->songs[song].length ||
            IsPlaying() || armed_ || launch_buffer_ || song_project_)
            return false;
        const auto& first = project->songs[song].entries[entry];
        if (first.pattern >= kMaxPatterns || !project->patterns[first.pattern].used ||
            !first.repeats)
            return false;
        song_slot_ = song;
        song_entry_ = entry;
        song_loop_ = loop;
        auto settings = SessionSettings();
        settings.tempo_bpm_x100 = project->songs[song].tempo_bpm_x100;
        settings.command = Protocol::SEQ_TRANSPORT_PLAY;
        ApplyTransport(settings);
        song_project_ = project;
        InstallSongEntry();
        return true;
    }
    void StopSong() {
        if (!song_project_)
            return;
        scheduler_.Stop();
        follower_.OnStop();
        armed_ = midi_enabled_ = awaiting_clock_ = false;
        *pending_pattern_ = song_project_->patterns[active_slot_].pattern;
        song_project_ = nullptr;
        ++pattern_revision_;
        AdvanceEpoch();
        CommitPendingPattern();
    }
    bool SongActive() const { return song_project_ != nullptr; }
    // One lock-free value for foreground status; section/repeat are zero/one based.
    uint32_t SongPosition() const {
        const uint8_t repeat =
            static_cast<uint8_t>(std::min<uint32_t>(255, scheduler_.PlayheadLoop() + 1));
        return active_slot_ | (static_cast<uint32_t>(song_entry_) << 8) |
               (static_cast<uint32_t>(repeat) << 16) | (SongActive() ? 1u << 24 : 0);
    }

    // ---- Transport + mode (MSG_SEQ_TRANSPORT) ----
    WAVEX_ITCM_CODE_NAMED("transport.ApplyTransport")
    void ApplyTransport(const Protocol::SeqTransportMessage& m) {
        if (song_project_ && (m.command == Protocol::SEQ_TRANSPORT_STOP ||
                              m.command == Protocol::SEQ_TRANSPORT_PLAY ||
                              m.command == Protocol::SEQ_TRANSPORT_CONTINUE))
            StopSong();
        tempo_bpm_ = static_cast<double>(m.tempo_bpm_x100) / 100.0;
        if (tempo_bpm_ < 1.0)
            tempo_bpm_ = 1.0;
        scheduler_.SetTempo(static_cast<float>(tempo_bpm_));
        follower_.SetNominalBpm(static_cast<float>(tempo_bpm_));

        if (input_mode_ != m.input_mode || m.command != Protocol::SEQ_TRANSPORT_CONFIGURE)
            recorder_.Reset();
        input_mode_ = m.input_mode;
        quantize_ = m.quantize;

        const bool want_midi = (m.clock_source == Protocol::SEQ_CLOCK_MIDI);
        follower_.SetSyncSource(want_midi);
        if (using_midi_ != want_midi) {
            StopSong();
            scheduler_.Stop();
            follower_.OnStop();
            armed_ = false;
            source_ = -1;
            source_sequence_valid_ = false;
            midi_enabled_ = awaiting_clock_ = false;
            if (output_playing_)
                EmitClock(Protocol::MIDI_CLK_STOP);
            output_playing_ = false;
        }
        using_midi_ = want_midi;

        switch (m.command) {
            case Protocol::SEQ_TRANSPORT_STOP:
                scheduler_.Stop();
                follower_.OnStop();
                armed_ = false;
                midi_enabled_ = awaiting_clock_ = false;
                source_ = -1;
                break;
            case Protocol::SEQ_TRANSPORT_PLAY:
                CommitPendingPattern();
                if (using_midi_) {
                    // Arm: wait for MIDI START to align step 0 to the downbeat.
                    armed_ = midi_enabled_ = true;
                    awaiting_clock_ = false;
                    source_ = -1;
                    source_sequence_valid_ = false;
                    scheduler_.Stop();
                } else {
                    scheduler_.Start();
                    follower_.OnStart();
                    pending_output_ = 1;
                    armed_ = false;
                }
                break;
            case Protocol::SEQ_TRANSPORT_CONTINUE:
                CommitPendingPattern();
                position_ = std::min<uint16_t>(m.song_position, 0x3fff);
                if (using_midi_) {
                    armed_ = midi_enabled_ = true;
                    awaiting_clock_ = false;
                    source_ = -1;
                    source_sequence_valid_ = false;
                    scheduler_.Stop();
                } else {
                    scheduler_.Seek(static_cast<double>(position_) * 24.0);
                    follower_.OnContinue(position_);
                    pending_output_ = 2;
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
        if (song_project_)
            return;
        using namespace Protocol;
        switch (m.op) {
            case SEQ_OP_RECORD_TARGET:
                if (StepValid(m.track, m.step))
                    recorder_.Target(m.track, m.step);
                return;
            case SEQ_OP_SET_MELODIC:
                if (m.track >= kMaxTracks || m.arg_u8 > 1)
                    return;
                pending_pattern_->tracks[m.track].melodic = m.arg_u8 != 0;
                break;
            case SEQ_OP_SET_NOTE_LANE:
                if (!StepValid(m.track, m.step) || m.arg_u8 >= kNoteLanes ||
                    (m.arg_u16 & 0xff) > 127 || (m.arg_u16 >> 8) > 127 || m.arg_s16 < 0)
                    return;
                recorder_.InvalidateLane(m.track, m.step, m.arg_u8);
                pending_pattern_->tracks[m.track].steps[m.step].notes[m.arg_u8] = {
                    static_cast<uint8_t>(m.arg_u16),
                    static_cast<uint8_t>(m.arg_u16 >> 8),
                    static_cast<uint16_t>(m.arg_s16)};
                if (m.arg_u16 >> 8)
                    pending_pattern_->tracks[m.track].steps[m.step].on = true;
                break;
            case SEQ_OP_SET_STEP:
                if (StepValid(m.track, m.step)) {
                    Step& s = pending_pattern_->tracks[m.track].steps[m.step];
                    s.on = (m.arg_u8 != 0);
                    s.velocity = ClampVelocity(m.arg_u16);
                }
                break;
            case SEQ_OP_SET_STEP_NOTE:
                if (StepValid(m.track, m.step) && m.arg_u8 <= 127)
                    pending_pattern_->tracks[m.track].steps[m.step].note = m.arg_u8;
                break;
            case SEQ_OP_TOGGLE_STEP:
                if (StepValid(m.track, m.step)) {
                    Step& s = pending_pattern_->tracks[m.track].steps[m.step];
                    s.on = !s.on;
                }
                break;
            case SEQ_OP_SET_STEP_PROB:
                if (StepValid(m.track, m.step)) {
                    pending_pattern_->tracks[m.track].steps[m.step].probability =
                        m.arg_u8 > 100 ? 100 : m.arg_u8;
                }
                break;
            case SEQ_OP_SET_STEP_MICRO:
                if (StepValid(m.track, m.step)) {
                    Step& s = pending_pattern_->tracks[m.track].steps[m.step];
                    s.retrig_count = m.arg_u8 > kMaxRetrigCount ? kMaxRetrigCount : m.arg_u8;
                    s.retrig_rate_ticks = static_cast<uint8_t>(m.arg_u16 & 0xFF);
                    s.micro_offset = m.arg_s16;
                }
                break;
            case SEQ_OP_TRACK_MUTE:
                if (m.track < kMaxTracks)
                    pending_pattern_->tracks[m.track].enabled = (m.arg_u8 != 0);
                break;
            case SEQ_OP_PATTERN_LENGTH: {
                uint16_t len = m.arg_u16;
                if (len < 1)
                    len = 1;
                if (len > kMaxSteps)
                    len = kMaxSteps;
                pending_pattern_->length = static_cast<uint8_t>(len);
                break;
            }
            case SEQ_OP_PATTERN_SCALE:
                if (m.arg_u8 <= static_cast<uint8_t>(StepScale::EighthTriplet))
                    pending_pattern_->scale = static_cast<StepScale>(m.arg_u8);
                break;
            case SEQ_OP_PATTERN_SWING: {
                uint8_t sw = m.arg_u8;
                if (sw < 50)
                    sw = 50;
                if (sw > 75)
                    sw = 75;
                pending_pattern_->swing = sw;
                break;
            }
            case SEQ_OP_SET_PARAM_LOCK:
                if (StepValid(m.track, m.step) && IsVoiceLockParameter(m.arg_u8))
                    SetParamLock(
                        pending_pattern_->tracks[m.track].steps[m.step], m.arg_u8, m.arg_u16);
                break;
            case SEQ_OP_CLEAR_TRACK:
                if (m.track < kMaxTracks) {
                    for (auto& step: pending_pattern_->tracks[m.track].steps)
                        step = Step{};
                }
                break;
            case SEQ_OP_SET_PARAM_LOCK_SLOT:
                if (StepValid(m.track, m.step) && m.arg_s16 >= 0 && m.arg_s16 < kMaxParamLocks &&
                    (m.arg_u8 == 0 || IsVoiceLockParameter(m.arg_u8))) {
                    auto& locks = pending_pattern_->tracks[m.track].steps[m.step].param_locks;
                    bool duplicate = false;
                    for (int i = 0; i < kMaxParamLocks; ++i)
                        duplicate |=
                            m.arg_u8 != 0 && i != m.arg_s16 && locks[i].param_id == m.arg_u8;
                    if (!duplicate)
                        locks[m.arg_s16] = {m.arg_u8, m.arg_u8 ? m.arg_u16 : uint16_t{0}};
                }
                break;
            case SEQ_OP_CLEAR_PARAM_LOCK:
                if (StepValid(m.track, m.step)) {
                    for (auto& lock: pending_pattern_->tracks[m.track].steps[m.step].param_locks)
                        if (lock.param_id == m.arg_u8)
                            lock = ParamLock{};
                }
                break;
            case SEQ_OP_CLEAR_PARAM_LOCKS:
                if (StepValid(m.track, m.step)) {
                    Step& s = pending_pattern_->tracks[m.track].steps[m.step];
                    for (auto& lock: s.param_locks)
                        lock = ParamLock{};
                }
                break;
            default:
                break;
        }
        pending_pattern_dirty_ = true;
        ++pattern_revision_;
    }

    bool ApplySlotEdit(const Protocol::SeqSlotEditMessage& message) {
        if (song_project_ || !Protocol::IsValidSeqSlotEdit(message) ||
            message.pattern != active_slot_ || message.epoch != active_epoch_)
            return false;
        ApplyPatternOp(message.edit);
        return true;
    }
    void BuildSlotPage(const Protocol::SeqPatternRequestMessage& request,
                       Protocol::SeqSlotPageMessage& page) const {
        page.read_only = SongActive();
        page.pattern = active_slot_;
        page.epoch = active_epoch_;
        BuildPatternPage(request, page.page);
    }

    void RecordInput(
        uint8_t source, uint8_t note, uint32_t serial, uint8_t velocity, uint16_t tracks) {
        SyncRecorderEpoch();
        if (!song_project_ && recorder_.Input(*pending_pattern_,
                                              input_mode_,
                                              quantize_,
                                              IsPlaying(),
                                              scheduler_.PatternPositionTicks(),
                                              source,
                                              note,
                                              serial,
                                              velocity,
                                              tracks)) {
            pending_pattern_dirty_ = true;
            ++pattern_revision_;
        }
    }
    template <typename Released>
    void PruneRecording(Released released) {
        recorder_.Prune(released);
    }
    void BuildNotes(const Protocol::SeqPatternRequestMessage& request,
                    Protocol::SeqNotesMessage& reply) const {
        reply.request_id = request.request_id;
        reply.epoch = active_epoch_;
        reply.revision = pattern_revision_;
        reply.pattern = active_slot_;
        reply.track = request.track;
        reply.step = request.first_step;
        reply.read_only = SongActive();
        reply.input_mode = input_mode_;
        reply.quantize = quantize_;
        const auto& row = pattern().tracks[request.track];
        reply.melodic = row.melodic;
        reply.clock_source = using_midi_ ? 1 : 0;
        reply.playing = IsPlaying();
        reply.tempo_bpm_x100 = SessionSettings().tempo_bpm_x100;
        reply.record_step = recorder_.StepIndex();
        for (uint8_t i = 0; i < kNoteLanes; ++i) {
            const auto& n = row.steps[request.first_step].notes[i];
            reply.notes[i] = {n.note, n.velocity, n.gate_ticks};
        }
    }

    // ---- MIDI clock (MSG_MIDI_CLOCK_EVENT) ----
    void OnMidiClock(const Protocol::MidiClockEventMessage& m) {
        using namespace Protocol;
        if (!using_midi_ || !IsValidMidiClockEvent(m))
            return;
        // First active source wins. Never switch a running/freewheeling song
        // merely because a second port is connected. Local re-arm releases it.
        if (source_ < 0) {
            source_ = m.source;
            source_sequence_valid_ = false;
        }
        if (source_ != m.source)
            return;
        switch (m.event) {
            case MIDI_CLK_TICK: {
                uint16_t gap = source_sequence_valid_
                                   ? static_cast<uint16_t>(m.tick_seq - source_sequence_)
                                   : 1;
                if (!gap || gap >= 0x8000)
                    return;
                source_sequence_ = m.tick_seq;
                source_sequence_valid_ = true;
                if (awaiting_clock_ && midi_enabled_) {
                    if (!LocatePosition(position_)) {
                        awaiting_clock_ = armed_ = false;
                        return;
                    }
                    follower_.OnMidiClock(m.esp_delta_us, gap, true);
                    follower_.OnContinue(position_);
                    awaiting_clock_ = armed_ = false;
                    break;
                }
                follower_.OnMidiClock(m.esp_delta_us, gap, true);
                break;
            }
            case MIDI_CLK_START:
                if (midi_enabled_) {
                    position_ = 0;
                    scheduler_.Stop();
                    follower_.OnStop();
                    armed_ = awaiting_clock_ = true;
                    source_sequence_valid_ = false;
                }
                break;
            case MIDI_CLK_CONTINUE:
                if (midi_enabled_) {
                    scheduler_.Stop();
                    follower_.OnStop();
                    armed_ = awaiting_clock_ = true;
                    source_sequence_valid_ = false;
                }
                break;
            case MIDI_CLK_STOP:
                if (scheduler_.IsPlaying())
                    position_ =
                        static_cast<uint16_t>(std::min(16383.0, scheduler_.PositionTicks() / 24.0));
                scheduler_.Stop();
                follower_.OnStop();
                awaiting_clock_ = false;
                armed_ = midi_enabled_;  // preserve a paused Song's immutable lease
                break;
            case MIDI_CLK_SPP:
                // SPP is a locate while stopped, never an implicit play command.
                if (!scheduler_.IsPlaying()) {
                    position_ = m.spp_beats16;
                    follower_.Locate(position_);
                }
                break;
            default:
                break;
        }
    }

    // Advance exactly one control tick. In MIDI mode, first advances the
    // follower and syncs the scheduler tempo to its servo-corrected rate.
    // Returns trigger events for this block (see SequencerScheduler::Process).
    WAVEX_ITCM_CODE_NAMED("transport.Tick")
    size_t Tick(TriggerEvent* out_events, size_t max_events) {
        SyncRecorderEpoch();
        if (!scheduler_.IsPlaying() && pending_pattern_dirty_) {
            CommitPendingPattern();
        }
        clock_frame_ += block_size_;
        clock_frame_published_.store(clock_frame_, std::memory_order_relaxed);
        if (using_midi_) {
            scheduler_.SetTempo(static_cast<float>(follower_.InstantaneousBpm()));
            follower_.Tick();
        }
        ServiceClockOutput();
        if (song_project_ && scheduler_.IsPlaying() && !scheduler_.HasQueuedPattern())
            ArmSongBoundary();
        if (!song_project_ && input_mode_ == Protocol::SEQ_INPUT_LIVE_ERASE &&
            scheduler_.IsPlaying()) {
            // Erase the upcoming nominal grid step before the scheduler reads it.
            const auto interval = StepIntervalTicks(pending_pattern_->scale);
            const auto step = static_cast<uint8_t>(
                static_cast<uint64_t>((scheduler_.PatternPositionTicks() +
                                       scheduler_.BlockEndTick() - scheduler_.PositionTicks()) /
                                      interval) %
                pending_pattern_->length);
            if (recorder_.Erase(*pending_pattern_, step)) {
                active_pattern_.tracks[recorder_.Track()].steps[step] =
                    pending_pattern_->tracks[recorder_.Track()].steps[step];
                pending_pattern_dirty_ = true;
                ++pattern_revision_;
            }
        }
        const size_t count = scheduler_.Process(out_events, max_events);
        if (song_project_) {
            if (scheduler_.SwitchedPattern()) {
                song_entry_ = static_cast<uint8_t>(song_entry_ + 1);
                if (song_entry_ == song_project_->songs[song_slot_].length)
                    song_entry_ = 0;
                InstallSongEntry();
                ArmSongBoundary();
            } else if (!scheduler_.IsPlaying() && !armed_) {
                StopSong();
            }
        }
        if (launch_buffer_) {
            if (scheduler_.SwitchedPattern()) {
                SwapWorkingPattern(*launch_buffer_);
                launch_buffer_ = nullptr;
            } else if (!scheduler_.HasQueuedPattern()) {
                launch_buffer_ = nullptr;
                launch_cancelled_ = true;
            }
        }
        if (pending_pattern_dirty_ && scheduler_.ProcessedStepBoundary()) {
            CommitPendingPattern();
        }
        return count;
    }

    // Main-loop consumer. Stale ticks cannot become a catch-up burst after SD I/O.
    bool PopClockOut(Protocol::SeqClockOutMessage& out) {
        // Latest transport state has priority over a congested clock ring.
        // A newer Start/Continue/Stop invalidates clocks from the old run.
        const auto control = output_control_.load(std::memory_order_acquire);
        if (control != output_control_seen_) {
            output_control_seen_ = control;
            output_continue_pending_ = (control & 3) == 2;
            const uint8_t event = (control & 3) == 1   ? Protocol::MIDI_CLK_START
                                  : (control & 3) == 2 ? Protocol::MIDI_CLK_SPP
                                                       : Protocol::MIDI_CLK_STOP;
            out = {event,
                   0,
                   static_cast<uint16_t>(event == Protocol::MIDI_CLK_SPP ? (control >> 2) & 0x3fff
                                                                         : 0)};
            return true;
        }
        if (output_continue_pending_) {
            output_continue_pending_ = false;
            out = {Protocol::MIDI_CLK_CONTINUE, 0, 0};
            return true;
        }
        ClockEntry entry;
        for (unsigned i = 0; i < 64 && clock_out_.Pop(entry); ++i) {
            if (entry.control != control ||
                clock_frame_published_.load(std::memory_order_relaxed) - entry.frame >
                    sample_rate_ / 20)
                continue;
            out = entry.message;
            return true;
        }
        return false;
    }
    // Foreground only: retry the latest transport state after link congestion.
    // Ticks expire/drop; a failed SPP/Continue restarts the ordered pair.
    void ClockOutFailed(const Protocol::SeqClockOutMessage& message) {
        if (message.event != Protocol::MIDI_CLK_TICK) {
            output_control_seen_ = 0;
            output_continue_pending_ = false;
        }
    }
    uint32_t ClockOutputDrops() const { return clock_out_.Dropped(); }

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
        uint16_t bpm_x100 = static_cast<uint16_t>(std::clamp(bpm * 100.0 + 0.5, 0.0, 65535.0));
        return Protocol::SeqPlayheadMessage(active_slot_,
                                            scheduler_.PlayheadStep(),
                                            scheduler_.IsPlaying() ? 1 : 0,
                                            sync_state,
                                            bpm_x100,
                                            scheduler_.PlayheadLoop());
    }

    // Callback-owned readback. No foreground reader touches either Pattern.
    void BuildPatternPage(const Protocol::SeqPatternRequestMessage& request,
                          Protocol::SeqPatternSyncMessage& out) const {
        static_assert(kMaxTracks == Protocol::SEQ_TRACK_COUNT &&
                          kMaxSteps == Protocol::SEQ_MAX_STEPS &&
                          kMaxParamLocks == Protocol::SEQ_STEP_LOCKS,
                      "wire/model bounds agree");
        out = Protocol::SeqPatternSyncMessage{};
        out.request_id = request.request_id;
        out.track = request.track;
        out.first_step = request.first_step;
        if (!Protocol::IsValidSeqPatternRequest(request))
            return;
        out.valid = 1;
        out.length = pattern().length;
        out.scale = static_cast<uint8_t>(pattern().scale);
        out.swing = pattern().swing;
        out.enabled = pattern().tracks[request.track].enabled;
        out.clock_source = using_midi_ ? Protocol::SEQ_CLOCK_MIDI : Protocol::SEQ_CLOCK_INTERNAL;
        out.input_mode = input_mode_;
        out.quantize = quantize_;
        out.tempo_bpm_x100 = static_cast<uint16_t>(tempo_bpm_ * 100.0 + 0.5);
        for (uint8_t i = 0; i < Protocol::SEQ_PAGE_STEPS; ++i) {
            const auto& step = pattern().tracks[request.track].steps[request.first_step + i];
            auto& wire = out.steps[i];
            wire.on = step.on;
            wire.velocity = step.velocity;
            wire.note = step.note;
            wire.probability = step.probability;
            wire.micro_offset = step.micro_offset;
            wire.retrig_count = step.retrig_count;
            wire.retrig_rate_ticks = step.retrig_rate_ticks;
            for (uint8_t k = 0; k < kMaxParamLocks; ++k) {
                wire.locks[k].parameter = step.param_locks[k].param_id;
                wire.locks[k].value = step.param_locks[k].value;
            }
        }
    }

    bool IsPlaying() const { return scheduler_.IsPlaying(); }
    bool IsArmed() const { return armed_; }
    bool UsingMidiSync() const { return using_midi_; }
    SyncLockState SyncState() const { return follower_.State(); }
    double TempoBpm() const { return tempo_bpm_; }
    uint8_t InputMode() const { return input_mode_; }

    // MIDI-CC observation (for the not-yet-built mod layer / tests).

    // Direct core access for tests that want to assert on the underlying
    // scheduler/follower state.
    const SequencerScheduler& scheduler() const { return scheduler_; }
    const TempoFollower& follower() const { return follower_; }

   private:
    void EmitClock(uint8_t event, uint16_t spp = 0) {
        auto control = output_control_.load(std::memory_order_relaxed);
        if (event == Protocol::MIDI_CLK_TICK) {
            ++output_sequence_;
            clock_out_.Push({{event, output_sequence_, 0}, clock_frame_, control});
        } else if (event != Protocol::MIDI_CLK_SPP) {
            const uint32_t kind = event == Protocol::MIDI_CLK_START      ? 1
                                  : event == Protocol::MIDI_CLK_CONTINUE ? 2
                                                                         : 3;
            control = ((control + 0x10000u) & 0xffff0000u) | (uint32_t(spp) << 2) | kind;
            output_control_.store(control, std::memory_order_release);
        }
    }
    void ServiceClockOutput() {
        if (using_midi_) {
            pending_output_ = 0;
            return;
        }  // no clock THRU/feedback loop
        if (!scheduler_.IsPlaying()) {
            if (output_playing_)
                EmitClock(Protocol::MIDI_CLK_STOP);
            output_playing_ = false;
            pending_output_ = 0;
            return;
        }
        if (pending_output_ || !output_playing_) {
            if (pending_output_ == 2) {
                EmitClock(Protocol::MIDI_CLK_CONTINUE, position_);
            } else
                EmitClock(Protocol::MIDI_CLK_START);
            next_output_tick_ = scheduler_.PositionTicks();
            output_playing_ = true;
            pending_output_ = 0;
        }
        // 4 internal ticks = one 24-PPQN MIDI clock. Absolute scheduler phase
        // supplies tempo changes without a second independently drifting clock.
        for (unsigned i = 0; i < 16 && next_output_tick_ < scheduler_.BlockEndTick(); ++i) {
            EmitClock(Protocol::MIDI_CLK_TICK);
            next_output_tick_ += 4.0;
        }
    }
    bool LocatePosition(uint16_t spp) {
        const double tick = static_cast<double>(spp) * 24.0;
        if (!song_project_) {
            scheduler_.Seek(tick);
            return true;
        }
        const auto& song = song_project_->songs[song_slot_];
        double total = 0;
        for (uint8_t i = 0; i < song.length; ++i) {
            const auto& entry = song.entries[i];
            const auto& pattern = song_project_->patterns[entry.pattern].pattern;
            total += pattern.length * StepIntervalTicks(pattern.scale) * entry.repeats;
        }
        if (total <= 0 || (!song_loop_ && tick >= total)) {
            StopSong();
            return false;
        }
        const double local = song_loop_ ? std::fmod(tick, total) : tick;
        double origin = 0;
        for (uint8_t i = 0; i < song.length; ++i) {
            const auto& entry = song.entries[i];
            const auto& pattern = song_project_->patterns[entry.pattern].pattern;
            const double length = pattern.length * StepIntervalTicks(pattern.scale) * entry.repeats;
            if (local < origin + length) {
                song_entry_ = i;
                InstallSongEntry();
                scheduler_.Seek(tick, tick - local + origin);
                return true;
            }
            origin += length;
        }
        return false;
    }

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

    void InstallSongEntry() {
        active_slot_ = song_project_->songs[song_slot_].entries[song_entry_].pattern;
        ++pattern_revision_;
        AdvanceEpoch();
        pending_pattern_dirty_ = false;
        scheduler_.SetPattern(&song_project_->patterns[active_slot_].pattern);
    }
    void ArmSongBoundary() {
        const auto& song = song_project_->songs[song_slot_];
        const auto repeats = static_cast<uint8_t>(
            std::max(1u,
                     unsigned(song.entries[song_entry_].repeats) -
                         std::min<unsigned>(song.entries[song_entry_].repeats - 1u,
                                            scheduler_.PlayheadLoop())));
        const auto next = static_cast<uint16_t>(song_entry_ + 1);
        if (next == song.length && !song_loop_)
            scheduler_.QueueStop(repeats);
        else {
            const auto slot = song.entries[next == song.length ? 0 : next].pattern;
            scheduler_.QueuePattern(&song_project_->patterns[slot].pattern, repeats);
        }
    }
    void AdvanceEpoch() {
        recorder_.Reset();
        if (!++active_epoch_)
            ++active_epoch_;
    }
    void SwapWorkingPattern(Pattern*& buffer) {
        AdvanceEpoch();
        std::swap(pending_pattern_, buffer);
        ++pattern_revision_;
        active_slot_ = launch_slot_;
        CommitPendingPattern();
    }

    void CommitPendingPattern() {
        active_pattern_ = *pending_pattern_;
        pending_pattern_dirty_ = false;
        scheduler_.SetPattern(&active_pattern_);
    }

    const Project* song_project_ = nullptr;
    uint8_t song_slot_ = 0, song_entry_ = 0;
    bool song_loop_ = false;
    void SyncRecorderEpoch() {
        if (recorder_run_epoch_ != scheduler_.RunEpoch()) {
            recorder_.Reset();
            recorder_run_epoch_ = scheduler_.RunEpoch();
        }
    }
    uint32_t recorder_run_epoch_ = 0;
    NoteRecorder recorder_;
    Pattern pending_storage_;
    Pattern* pending_pattern_ = &pending_storage_;
    Pattern** launch_buffer_ = nullptr;
    uint8_t launch_slot_ = 0, active_slot_ = 0;
    bool launch_cancelled_ = false;
    Pattern active_pattern_;
    bool pending_pattern_dirty_ = false;
    uint32_t pattern_revision_ = 0, active_epoch_ = 1;
    SequencerScheduler scheduler_;
    TempoFollower follower_;

    struct ClockEntry {
        Protocol::SeqClockOutMessage message;
        uint32_t frame, control;
    };
    Midi::EventRing<ClockEntry, 64> clock_out_;
    std::atomic<uint32_t> clock_frame_published_{0}, output_control_{0};
    // Foreground-owned cursor for the transport mailbox.
    uint32_t output_control_seen_ = 0;
    bool output_continue_pending_ = false;
    uint32_t clock_frame_ = 0;
    double next_output_tick_ = 0;
    uint16_t output_sequence_ = 0, position_ = 0, source_sequence_ = 0;
    int8_t source_ = -1;
    uint8_t pending_output_ = 0;
    bool source_sequence_valid_ = false, midi_enabled_ = false, awaiting_clock_ = false,
         output_playing_ = false;

    uint32_t sample_rate_ = 48000;
    uint16_t block_size_ = 48;
    double tempo_bpm_ = 120.0;
    bool using_midi_ = false;
    bool armed_ = false;  // MIDI mode: armed or externally paused
    uint8_t input_mode_ = 0;
    uint8_t quantize_ = 0;
};

}  // namespace Sequencer
}  // namespace WaveX
