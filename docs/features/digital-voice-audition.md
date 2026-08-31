# Digital Voice Audition — Playable Grid, Live Params, Sequenced Playback

**Status**: Stages 1, 3 and 4 done (grid page, live params, playable from the front panel; unverified on hardware). Stage 2 (root-note correctness) and Stage 2b (self-reporting, added 2026-08-31 from the bench session) are still open, so Goal A is not yet complete. Goal B (Stages 5–8, the sequencer path) not started. Consolidates the near-term path through Phase 2, borrowing narrowly from Phase 2.5.
**Dependencies**: `VoiceManager` + `Envelope` (built, host-tested), `Instrument::ResolveNoteOn` (built, host-tested), sequencer cores `pattern.hpp` / `sequencer_scheduler.hpp` / `sequencer_transport.hpp` (built, host-tested), `MSG_NOTE_ON` transport (built, in service via MIDI).
**Scope decision (2026-08-29)**: **all-digital sound engine**. The Stage A analog VCF/VCA path (`cv_group_router.hpp`, `ParaphonicParams`, `paraphonic_envelope.hpp`) is out of scope here and is neither removed nor extended — it keeps working, it is simply not the path this document builds on.

---

## 1. Why this document exists

The work needed to *play a loaded sample at pitch and hear voice edits while it sounds* is currently spread across five documents (`sequencer.md`, `melodic-sequencing.md`, `param-locks-and-modulation.md`, `instrument-model.md`, `roadmap.md` Phases 2 and 2.5), interleaved with work that is not needed for it — multisampling, MIDI clock out, kits, songs, live record. This document is the single ordered path, and it deliberately takes the smallest slice of each of those designs that the goal actually requires.

Two goals, in order:

- **Goal A** — play the loaded sample chromatically from an on-screen grid, and sweep filter/envelope while it sounds. Stages 1–4.
- **Goal B** — program a step pattern, run it, and edit voice parameters live while it plays. Stages 5–8.

## 2. The finding that shapes the order

**At the start of this work, the only live-editable parameter path was the analog one this scope excludes.** `MSG_CONTROL_CHANGE` for cutoff / resonance / ADSR was consumed in `audio_engine.cpp` (`OnControlChange`) into the paraphonic state and `s_para_env`, staged to the MCP4728 at the 1 kHz control tick. That was genuinely real-time audible — and it was the analog path.

The digital per-voice chain is real DSP (`voice_manager.hpp`: each of `kNumVoices = 8` voices owns its own filter and `Envelope`, processed inline in `Render()`), but its cutoff and ADSR are written **once, at `Trigger()` time**, from `VoiceTriggerParams`. There is no message, no API, and no state that lets a control change reach either a sounding voice or the next note to be triggered.

Consequence: on an all-digital engine, turning a filter knob currently does nothing. "Audition while editing" is therefore not a UI feature layered on top — it is missing engine plumbing, and it is Stage 1 regardless of which UI surface gets built first.

## 3. Two features that are one widget

A 4×4 grid whose cell *n* sends MIDI note `root + n` **is** a chromatic keyboard spanning 16 semitones. A "virtual piano" and a "pad grid" differ only in the note-map and the cell drawing, not in the input handling, the message path, or the page scaffolding.

This is built once, as a grid widget with a swappable note-map:

- **Chromatic** — `note = root + index`, 16 semitones, root selectable. The piano case.
- **Pad/kit** — `note = pad_note[index]`, a table. The drum case, and later the kit case.

Nothing about the engine distinguishes them: both end in `inter_mcu_send_note_on(note, velocity, channel)`, which already exists (`firmware/esp32/main/inter_mcu.cpp`) and is already in service from `midi_task.cpp`. **No new protocol message is required for Goal A.**

## 4. Stages

One verified commit each, per project convention.

### Stage 1 — Live digital voice parameters — **DONE** (host-tested; unheard on hardware)

The load-bearing stage. Three parts:

1. ~~**A per-slot base-parameter record**~~ **Done, but engine-global rather than per-slot.** `VoiceLiveParams` is owned as `s_voice_live_pending` by `OnControlChange`, published as a complete snapshot, and read when building `VoiceTriggerParams`. Per-slot was designed for and deliberately not built: nothing can address a slot differently yet — `OnNoteOn` does not even set one — so a 16-entry table would have been 16 copies of the same values with no way to reach 15 of them. It mirrors `s_para_pending`, which is engine-global for the analog path for exactly the same reason, and it becomes per-slot with the instrument model (Phase 2.5), which is when a slot becomes addressable.
2. ~~**A live-update surface on `VoiceManager`**~~ **Done** — `ApplyLiveParams(const VoiceLiveParams&)`, driven at block rate from `Callback()` only when the mailbox publishes a new generation, so an unchanged parameter set costs nothing. `VoiceManager::Choke()` was the precedent for mutating a playing voice's DSP state from callback context, so this needed a parameter path, not permission.

   **One asymmetry was discovered while building it, and it is load-bearing.** Filter edits reach *every* sounding voice including those in their release tail — a sweep that froze at note-off would sound like the filter jammed. Envelope edits deliberately skip releasing voices, because `Choke()` forces a short release onto a voice immediately before releasing it, and rewriting the ADSR would hand that voice its full-length release back mid-choke: the open hat would not cut off. A host test pins this, and it was verified to fail without the guard rather than merely passing with it.
3. ~~**Routing**~~ **Done.** `PARAM_FILTER_CUTOFF`, `PARAM_FILTER_RESONANCE` and `PARAM_ENVELOPE_*` now have two destinations each — the analog `s_para_pending` and the digital `s_voice_live_pending`. They are not alternatives: the analog board is optional hardware and the digital voices always render, so a knob has to reach both. Cutoff needed a mapping the analog path does not (it passes `norm` through as a CV): exponential over 20 Hz – 20 kHz, since a linear map spends most of its travel above 10 kHz and crosses the whole musically useful range in the first few percent.

**Filter replacement (decided 2026-08-29; ~~done~~ — `svf_filter.hpp`, 14 host tests): `OnePoleFilter` → a state-variable filter.** `PARAM_FILTER_RESONANCE` has no digital consumer today because the one-pole has no resonance state — and a filter that cannot resonate barely exercises the voice architecture this work exists to exercise. `one_pole_filter.hpp`'s own header comment anticipates exactly this ("upgrading to an SVF (for resonance) is a drop-in follow-up once there's a reason to need it").

Constraints on that replacement:

- **Written locally and portably, not pulled from DaisySP.** `voice_manager.hpp` is explicitly HAL-free and host-testable without a cross compiler, and `one_pole_filter.hpp` is a local implementation for that reason. A new `svf_filter.hpp` follows the same rule.
- **Per-voice cost must be measured**, not asserted (AGENTS.md). An SVF is meaningfully more work per sample than a one-pole, ×8 voices, inside `Render()`'s inner loop. DWT before/after on hardware; if the budget is tight, that is a finding to report, not to absorb silently.
- **Keep cutoff-at-or-above-Nyquist as an exact bypass**, the behaviour the one-pole documents and its tests pin.
- **`Voice` grows, and `s_voice_manager` is DTCM-placed** (`WAVEX_DTCM_DATA`, measured at 872 B of the 128 KB region for 8 voices). SVF state is a handful of floats per voice, so this is not a budget concern — but the placement is deliberate and the growth should be re-checked against the linker report rather than assumed free.

### Stage 2 — Root-note correctness

`OnNoteOn` assigns a fixed `root_note = kDefaultRootNote` when building its trigger params — a named constant, but still a hardcoded assumption, not a resolved value. `Instrument::ResolveNoteOn` — which resolves note + velocity into `VoiceTriggerParams` through the zone model, including the zone's own root note, tuning and gain — is built and host-tested but is not wired into the live note path. Wire it. Without this, every sample is assumed to be recorded at C4 and anything else plays at the wrong pitch, which makes the whole grid misleading.

### Stage 2b — The instrument must be able to report why it is silent

Found at the 2026-08-29 bench session, and it is the reason Stage 2's pitch
bug was hard to even reach: tapping pads produced no sound *and no diagnosable
output*, so "nothing happened" could not be distinguished from "a note was
sent and dropped." Three distinct causes, none of which is the note path — that
path was traced end to end and is correct (pad → `pad_event_cb` → `press()` →
`inter_mcu_send_note_on` → UART → `HandleNoteMessage` → `AudioEngine::OnNoteOn`,
with `WAVEX_AUDIO_ENGINE_ENABLED` at 1).

1. **The silence is a logging artefact.** The ESP32 send path logs through
   `UART_LOGI`, and `uart_debug_config.h` sets `WAVEX_UART_DEBUG_LEVEL` to
   errors-only, so every `UART_LOGI` compiles out — the ESP32 is *expected* to
   be silent on a note send. The Daisy's "dropped (no playable sample)" line
   goes to the log ring drained over USB CDC, not the UART console, and is
   gated behind `if (s_hw)`. Folding these into the module table is already
   tracked in `../backlog.md`; until then, an absence of log output carries no
   information here.

2. ~~**A 24-bit sample loads successfully and can never be triggered.**~~
   **Fixed.** RAM-resident loading now accepts PCM16 mono/stereo only, matching
   the voice renderer's actual data contract. The browser gives an actionable
   error for known PCM24 metadata, and the Daisy independently validates the
   parsed WAV header so missing or stale frontend metadata cannot admit an
   unplayable allocation. **Audition** still streams PCM24 files.

3. **Audition is not Load, and nothing says so.** The browser's **Audition**
   softkey sends `MSG_SAMPLE_PLAY_INDEX_REQ`, which streams a file by listing
   index and never makes it RAM-resident; only **Load** sends
   `MSG_SAMPLE_LOAD`. A sample that was auditioned and not loaded is genuinely
   absent from `s_loaded_samples`, so the grid correctly plays nothing.

Compounding all three, the Play page's info line
(`ui_play_page.cpp:458`) unconditionally appends `(needs a 16-bit sample
loaded)` whether or not one is loaded. It reads as a status report and is not
one — replace it with real state derived from the `SampleMetadata` the
frontend already receives.

**Do this before Stage 2.** Root-note correctness is unobservable on an
instrument that cannot tell you whether a note was dropped, and every bench
attempt at Stage 2 will otherwise re-run this diagnosis from scratch.

### Stage 3 — The grid page — **DONE**

`UIPlayPage` (`ui_play_page.cpp`), registered under "Play" in the main menu (`ui_main_menu.cpp`). Keys use `LV_EVENT_PRESSED` → `inter_mcu_send_note_on`, `LV_EVENT_RELEASED` / `LV_EVENT_PRESS_LOST` → `inter_mcu_send_note_off`, matching the press/release-gated design this stage called for (`PRESS_LOST` covers the slid-off-finger case).

### Stage 4 — Live parameter editing on that page — **DONE**

`kParams[]` in `ui_play_page.cpp` pages through Cutoff, Resonance, Attack, Decay, Sustain, Release on the softkey row, each sending `MSG_CONTROL_CHANGE` via `inter_mcu_send_control_change` (also wired on `ui_voice_page.cpp`, the Voice group's live-edit surface). Stage 1 is what makes these audible.

**Goal A is not yet complete**, even though Stages 3–4 shipped ahead of Stage 2: root-note correctness is still open, so pitch is wrong for any sample not recorded at the default root — and Stage 2b means the instrument cannot yet report that, or any other, reason for silence.

### Stage 5 — The sequencer's audible half

Drive `SequencerTransport::Tick()` from the 1 kHz control tick and convert its `TriggerEvent` stream into `VoiceManager::Trigger()` calls at the event's intra-block frame offset.

`audio_engine.cpp`'s `s_seq_transport` comment names two blockers for this. Their current status:

- **The track→sample kit mapping** it defers to the instrument model (Phase 2.5) **dissolves for this scope**: with one loaded sample at 16 pitches, track index maps to a semitone offset, which is the same class of policy `OnNoteOn` already applies. This is an honest interim, and it is the reason Goal B does not have to wait for Phase 2.5 — but it must be written as a *replaceable* mapping, not assumed permanent.
- **The double-buffered edit-between-steps discipline** (`sequencer.md` §4) is real and remains. Edits arrive on the main loop; `Tick()` runs in audio-callback context; today `SequencerTransport` is single-buffered and adds no locking (it has no HAL to lock with). Without this, editing a step while the pattern plays can tear the step the callback is mid-read of.

### Stage 6 — Playhead feedback

`SequencerTransport::BuildPlayhead()` already constructs a `SeqPlayheadMessage`. Emit it, coalesced to ≤ 30 Hz, and handle it on the ESP32. Per the cross-cutting rule, the UART RX task stores and flags; an `lv_timer` draws.

### Stage 7 — Step editor UI

The grid widget again, in step mode: 16 steps across, track selection, toggling a step sends `MSG_SEQ_PATTERN_OP`; transport sends `MSG_SEQ_TRANSPORT`.

**The backend for this is already live.** Both messages dispatch on the Daisy (`daisy_inter_mcu_message_handlers.cpp` → `AudioEngine::OnSeqPatternOp` / `OnSeqTransport` → `s_seq_transport`), and the transport applies them to a fully unit-tested pattern model. What is missing is entirely on the ESP32: no page, no widget, and no sender for any `MSG_SEQ_*` message exists.

`MSG_SEQ_PATTERN_SYNC` (0x52) is a reserved id whose struct is not yet defined — it is for bulk project load/save and is **not** needed by this stage. Do not define it here.

### Stage 8 — Parameter locks

`ApplyParamLocks(VoiceTriggerParams&, const ParamLock*, n)` per `param-locks-and-modulation.md` §2 — a pure, host-testable function applied at step-fire time. The pattern model already carries `param_locks[≤4]` per step and `TriggerEvent` already carries them through to the trigger site, so this is application logic, not model work.

**Goal B is complete at the end of this stage.**

## 5. Protocol deltas

**Goal A requires none.** `MSG_NOTE_ON` / `MSG_NOTE_OFF` and `MSG_CONTROL_CHANGE` with the existing `ControlParameter` ids (0x01–0x0A) cover Stages 1–4 end to end.

Goal B requires no new *messages* either — `MSG_SEQ_TRANSPORT` (0x50), `MSG_SEQ_PATTERN_OP` (0x51) and `MSG_SEQ_PLAYHEAD` (0x53) are defined, dispatched and round-trip tested. Stage 7 adds ESP32-side senders for messages that already exist, which is not a wire change.

Any change that *does* touch `protocol.h` carries its round-trip test and its `inter-mcu-protocol.md` row in the same commit (cross-cutting rule).

## 6. Test plan

- **Host, Stage 1**: SVF response (cutoff/resonance sweep, stability at extremes, Nyquist bypass); live parameter update reaching a sounding voice; base-params write→trigger path. Extends `voice_manager_test.cpp`, plus a new `svf_filter_test`.
- **Host, Stage 2**: note→pitch through `ResolveNoteOn` for non-60 root notes; the existing `instrument_test` covers resolution, so this is the wiring assertion.
- **Host, Stage 5**: `TriggerEvent` → trigger conversion, including intra-block frame offsets and the track→note mapping; concurrent-edit safety of the double buffer.
- **Host, Stage 8**: per-param-id lock mapping and clamping.
- **Hardware** (cannot be host-tested, and must be reported as unverified until run): audible pitch accuracy across the 16 cells; touch-to-sound latency; a filter sweep while holding notes with no zipper noise or dropout; **DWT before/after for the SVF swap**, 8 voices active; a zero-underrun soak with the sequencer running.

## 7. Deliberately out of scope

Named so they are not half-built by accident:

- The Stage A analog path (per the scope decision above) — untouched, still building.
- Multisampling, key/velocity zones beyond what `ResolveNoteOn` already does, kits, songs, project persistence.
- MIDI clock out, live record, step record, arpeggiator.
- The modulation matrix and LFOs (`param-locks-and-modulation.md` §3–5). Stage 1 shapes the voice's live-parameter surface to match what the matrix will later drive, but builds no matrix, no LFO and no second envelope.
- `MSG_SEQ_PATTERN_SYNC` / bulk pattern transfer.
