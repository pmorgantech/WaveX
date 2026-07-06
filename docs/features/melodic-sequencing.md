# Melodic Sequencing — Note Tracks, Chords, Live Record

**Status**: Target design (unimplemented). Phase 2.5 in `roadmap.md`.
**Dependencies**: Phase 2 sequencer core (`sequencer.md` — scheduler, pattern model, transport), `instrument-model.md` (keyboard-mode instruments are what melodic tracks play).
**Lineage**: Emax's built-in sequencer recorded keyboard performances over its preset engine; that combination — multisampled instrument + note sequencer + analog voice path — is the target here.

---

## 1. One pattern model, two track types

`sequencer.md` §3 defines `Tracks[≤16]` with drum-shaped steps. Rather than a parallel system, tracks gain a **type**:

- **Drum track** (existing design): step = `{on, velocity, probability, micro_offset, retrig, param_locks}` and the track binds a pad/zone. Unchanged.
- **Melodic track**: same step scaffold, plus per-step **note content**; the track binds an **instrument slot** (`instrument-model.md` §1) and pitch flows through to zone resolution.

```cpp
// Extends sequencer.md §3's Step for type==MELODIC tracks
struct StepNotes {                 // present only on melodic tracks (parallel array,
    struct {                       // not a union in Step — keeps drum Step size unchanged)
        uint8_t note;              // 0 = empty lane
        uint8_t velocity;
        uint16_t length_ticks;     // gate length, internal 96-PPQN ticks (0 = legato/tie)
    } lane[4];                     // ≤ 4 simultaneous notes per step (chord)
};
```

Memory: 4 lanes × 4 B × 64 steps × 16 tracks = 16 KB worst case per pattern if *every* track were melodic — acceptable; allocate `StepNotes` arrays per-track only when the track is melodic (fixed pool of, say, 8 melodic-track buffers per pattern, `WAVEX_SEQ_MAX_MELODIC_TRACKS = 8`).

**Note-off scheduling**: drum steps are fire-and-forget; melodic notes need releases. The scheduler keeps a small sorted pending-off queue (≤ 32 entries: 8 tracks × 4 lanes): when a step fires, push `(frame + length_ticks·frames_per_tick, note, slot)`; the control tick drains due entries into `VoiceManager::Release`. `length_ticks == 0` means **tie**: no off is scheduled and the next step on the same lane retriggers legato (v1: retrigger; true glide/portamento is a later voice-manager feature — noted in §5).

## 2. Editing surfaces

1. **Step editor extension** (`sequencer.md` §5.2): on a melodic track, holding a step opens the note lane view — 4 lanes × (note, velocity, length). Encoder A = pitch (constrained by the active scale mask, `tuning-and-scales.md` §4), B = length, C = velocity.
2. **Step-record mode** (Emax workflow): sequencer stopped or looping, hold a step and play notes on MIDI/pads — incoming `MSG_NOTE_ON` fills lanes of the held step. ESP-side interaction, emits normal pattern-op messages; the Daisy needs a "route incoming notes to the editor, not the engine" toggle → `SEQ_PATTERN_OP` gains `SEQ_OP_SET_STEP_NOTES {track, step, StepNotes}` and the routing toggle rides `MSG_SEQ_TRANSPORT` (`input_mode`: 0=play, 1=step-record).
3. **Live record** (the important one — see §3).

## 3. Live recording into the pattern

Played notes get captured **on the Daisy**, because only the Daisy knows the musical time of an incoming note (ESP32 knows nothing about the playhead).

- While transport is playing and `input_mode == 2` (live-record), each `OnNoteOn` is (a) played immediately through the instrument path as normal, and (b) logged `(Φ_now, note, velocity, slot)`; `OnNoteOff` closes the pair giving `length_ticks`.
- **Quantize on capture** to the nearest step by default (`quantize`: off/step/half-step, transport field); micro-offset stores the residual when quantize is off — the same `micro_offset` field drum steps already have, so nothing new in the scheduler.
- Captured pairs are merged into the pattern between steps (the double-buffered row rule, `sequencer.md` §4) and echoed to the UI as ordinary step-change feedback so the grid lights up as you play. Overdub semantics: new notes fill empty lanes; a 5th note on a full step replaces the oldest lane (and the UI flashes the step).
- **Erase gesture**: hold a pad/key while record-armed and passing the playhead clears matching lanes (Elektron-style live erase) — one more `input_mode` value.

## 4. Protocol deltas (within the 0x50 block; round-trip tests same commit)

- `SEQ_PATTERN_OP` (0x51) new ops: `SET_TRACK_TYPE {track, type, instrument_slot}`, `SET_STEP_NOTES {track, step, StepNotes}` (20 B payload — fits the 64 B class), `CLEAR_STEP_NOTES`.
- `MSG_SEQ_TRANSPORT` (0x50): add `input_mode` (play / step-record / live-record / live-erase) and `quantize`.
- Playhead/step feedback (0x53) unchanged — melodic steps light the same way.

## 5. Explicitly deferred

- Portamento/glide and true legato (voice-level, needs a `Voice::GlideTo(note, time)`; design when asked).
- Per-lane probability (v1: step-level probability applies to the whole chord).
- Polyphonic aftertouch routing (waits on `param-locks-and-modulation.md` mod sources growing an aftertouch input).
- MIDI *output* of melodic tracks (sequencing external gear) — natural follow-on: emit lane events as `MSG_SEQ_CLOCK_OUT`-style note messages to the ESP32 MIDI out; reserve `SEQ_OP` value now, implement post-gate.

## 6. Test plan

- Host: golden scheduler tests — melodic pattern in, `(frame, NoteOn/NoteOff)` stream out; tie handling; pending-off queue overflow behavior (oldest-off forced early, never dropped silently); quantize math including wrap at pattern end.
- Host: live-record capture — synthetic `Φ`/note streams in, expected step contents out (quantize on/off, overdub replacement, erase).
- Hardware (Phase 2.5 gate): record a 2-bar 4-note chord progression live over a playing drum pattern, then a 10-minute loop soak — zero underruns, no stuck notes (every On has a matching Off after transport stop: add a transport-stop "flush pending offs + release all melodic voices" rule, host-tested).

## 7. Implementation stages (one verified commit each)

1. Pattern-model extension (`StepNotes` pool, track type) + scheduler note-off queue, host-tested golden streams.
2. Protocol ops + round-trip tests + `inter-mcu-protocol.md` rows.
3. Daisy live-record capture module (HAL-free, host-tested) + transport `input_mode` wiring + dispatch tests.
4. ESP32 step editor note-lane view + step-record mode.
5. Live-record UI (arm, quantize setting, step flash feedback); bench soak.
