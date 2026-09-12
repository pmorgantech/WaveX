# Parameter Locks & Modulation Matrix — Design

**Status**: Parameter-lock application and touch editing are implemented. Four
voice-scoped locks per step are applied after zone resolution; pattern files
retain them. The Instrument modulation editor, matrix, global LFO and second
envelope already exist. Expanded oscillator, envelope and per-voice LFO work
remains Phase 2.5. Live motion recording and analog/group locks are still target
design below, not implemented behavior.
**Dependencies**: sequencer step scheduler (Phase 2), `instrument-model.md` (matrix slots are instrument-scoped), voice manager (done). **Revised 2026-09-04**: the two-oscillator Instrument (`track-and-patch-model.md` §3.1) fixes the source/destination set this matrix serves — three envelopes, two per-voice LFOs, one global LFO, oscillator and wavetable-position destinations — appended to the enums below, never renumbered.
**Lineage**: two ancestries deliberately fused — Elektron parameter locks (per-step sound design) and the E-mu EIII **realtime controls matrix** (velocity/wheel/pedal → pitch, filter, level, LFO amount, attack — routed, not hardwired).

---

## 1. Parameter identity and mappings

The wire contract is `ControlParameter` in
[protocol.h](../../firmware/shared/spi_protocol/protocol.h). Existing PAN (0x08)
and PITCH (0x09) retain their identities. The earlier proposed duplicate ids are
withdrawn. SAMPLE_START, LOOP_START and GAIN occupy their reserved ids; macros
and analog/group controls remain reserved.

All locks carry a uint16 value. Cutoff maps exponentially from 20 Hz to 20 kHz;
resonance/sustain/pan map from zero to one; ADSR times map from 1 to 2001 ms.
Pitch replaces the live Track offset over -24 to +24 semitones while retaining
zone tuning. Gain scales the resolved zone gain, with 32768 meaning unity.
Start and loop start map within their original resolved ranges, leaving at least
two frames. Loop start does nothing when looping is disabled. Other voice
controls compose with the existing post-trigger modulation.

## Touch editing (as built)

Sequencer → Shift → Locks displays four slots above the grid. Drag Slot,
Parameter and Value to choose an override; dragging an occupied slot's value
edits it directly. Tapping a step selects it without toggling its note while in
Locks view. Clear lock removes only the selected override. Grid returns to the
ordinary step editor. An asterisk marks steps containing stored locks.

Slot replacement is one atomic pattern operation, rejects duplicate parameters,
and waits for backend readback. Live Instrument edits still update unlocked
fields on held voices; locked fields retain their trigger values. A subsequent
unlocked step resolves the Instrument normally. Master volume, global LFO,
macros and analog/group parameters are not offered as voice locks. Unsupported
ids in files are preserved but ignored by the renderer.

## 2. Parameter locks

Already in the pattern model (`sequencer.md` §3): `param_locks[≤4]{param_id, value}` per step. This doc pins the application path:

- At step-fire time the scheduler resolves the step's zone/notes into `VoiceTriggerParams` (via `instrument-model.md` §3) and then applies each lock **to the trigger params** for voice-scoped ids (cutoff, ADSR, pitch offset via `pitch_ratio_mul`, pan, gain via `gain_mul`, sample start as region offset). No global state is touched — a p-lock on step 5 cannot bleed into a live-played note. Host-testable pure function: `ApplyParamLocks(VoiceTriggerParams&, const ParamLock*, n)`.
- Locks on *track/group-scoped* ids (PARAM_ANALOG_CUTOFF, sends later) stage a control-tick value that reverts to base at the next step boundary of that track (Elektron behavior: the lock lasts one step). Implemented as `{value, revert_at_tick}` pairs in the track state.
- **Live p-lock recording**: transport in live-record, hold a step (or with record running, just turn a knob — "motion record lite"): incoming `MSG_CONTROL_CHANGE` while a step window is active writes a lock into that step (window = the step whose span contains `Φ_now`). Capped at 4 locks/step, oldest evicted with UI flash (same rule as melodic lane overflow).

## 3. Modulation matrix (instrument-scoped)

Stored in `Instrument` (`instrument-model.md` §2), 8 slots:

```cpp
struct ModSlot {
    uint8_t source;   // ModSource
    uint8_t dest;     // ControlParameter (voice- or group-scoped subset)
    int16_t depth;    // ±32767 → ±100%
    uint8_t curve;    // 0=linear, 1=exponential, 2=S-curve (LUT, 33 points)
    uint8_t flags;    // bit0: unipolar source remap to bipolar
};
enum ModSource : uint8_t {
    SRC_NONE, SRC_VELOCITY, SRC_NOTE,          // per-trigger, sampled once
    SRC_ENV_FILTER,                            // per-voice Env 2 (§4) — "Env 2" in the UI
    SRC_LFO1,                                  // THE global LFO (§5)
    SRC_LFO2,                                  // retired-but-reserved 2026-09-04: always 0 (one global LFO only)
    SRC_LFO_VOICE,                             // per-voice LFO 1 (§5)
    SRC_RANDOM,                                // per-trigger S&H, seeded RNG
    SRC_MACRO_1, SRC_MACRO_2, SRC_MACRO_3, SRC_MACRO_4,
    SRC_MODWHEEL, SRC_AFTERTOUCH,              // from MIDI CC1 / channel pressure
    SRC_PARA_ENV,                              // Stage A paraphonic envelope (analog dests)
    // Appended by the two-oscillator Instrument (track-and-patch-model.md §3.1); wire-stable, append-only:
    SRC_LFO_VOICE2,                            // per-voice LFO 2
    SRC_ENV1,                                  // amp envelope as a source
    SRC_ENV3,                                  // per-voice Env 3 (pitch by default)
};
// ModDest grows the same way: today's CUTOFF/GAIN/PITCH/PAN, then RESONANCE,
// OSC1_PITCH, OSC2_PITCH, OSC_MIX, OSC2_LEVEL, WT_POS1, WT_POS2, LFO1_RATE,
// LFO2_RATE — appended, never renumbered.
```

**Evaluation model — control-rate, never per-sample**: once per 1 ms control tick, for each active voice, `mod[dest] += depth · curve(source_value)` over its instrument's slots. Cost ceiling: 8 slots × 8 voices × ~10 ops = trivial. Per-trigger sources (velocity, note, random) are sampled into the voice at trigger and treated as constants. Destinations applied at block rate: cutoff → `filter.SetCutoff` once per block (one-pole recomputes its coefficient — cheap), gain/pan → block-constant multipliers, pitch → `increment` multiplier update once per block (this quantizes vibrato to 1 kHz steps, which is inaudible; do **not** move pitch mod per-sample without a DWT budget check).

This requires the modest `Voice` surface: `SetBlockModulation(cutoff_mul, gain_mul, pitch_mul, pan_offset)` applied at the top of its render slice — one struct write, callback-safe.

## 4. Second envelope (filter envelope)

E-mu voices had filter + amp envelopes; WaveX `Voice` has one (amp). Add `Envelope env2` to `Voice` (same linear `envelope.hpp` class, retrigger/release alongside env1), exposed only as `SRC_ENV_FILTER`. Zone carries its ADSR (defaults in `instrument-model.md` wire-Zone chunk v2 — append fields, bump chunk version). Cost: one more `Process()` per active voice per sample… **no** — env2 is a *mod source*, so it's evaluated **once per block** (32.32 phase advanced by block length), not per sample. Keeps the callback budget flat.

## 5. LFOs

Decided 2026-09-04 (`track-and-patch-model.md` §3.1, §9 item 16): **two
per-voice LFOs owned by the Instrument plus one engine-global LFO.** The
earlier "2 global + 1 per-voice" split is withdrawn; `SRC_LFO2` stays in the
enum as retired-but-reserved and reads 0.

- **1 global LFO** (control-tick, in `AudioEngine`, built as `SRC_LFO1`): sine/tri/saw/square/S&H, rate either Hz (0.02–20) or tempo-synced divisions (1/16 … 4 bars — needs the sequencer clock; free-runs in Hz until Phase 2 lands). Phase-restart options: free, on-transport-start, on-any-note. Engine-global like a modular's LFO bank: performance-wide movement, not part of any Instrument.
- **2 per-voice LFOs** (`SRC_LFO_VOICE`, `SRC_LFO_VOICE2`; not yet built): phase accumulator per voice per LFO, waveform/rate/delay/fade/retrigger from the Instrument (`Instrument::lfo[2]`, saved in the `LFO1`/`LFO2` chunks), evaluated per block. Retrigger at note-on with optional `delay_s` and `fade_s` (the classic E-mu delayed-vibrato envelope on the LFO — one ramp, two params). Their rates are mod destinations (`LFO1_RATE`, `LFO2_RATE`).
- Why the Instrument owns them: an `.wxi` must sound the same on any Track and in any Project. Slot 3's wobble must not change because slot 5 loaded a new Instrument, and it must not depend on an engine setting the file does not carry.

## 6. Protocol

- Matrix/LFO edits ride `MSG_INST_OP` (0x60): new ops `SET_MOD_SLOT {slot_index, ModSlot}`, `SET_VOICE_LFO {wave, rate, delay, fade}`, `SET_GLOBAL_LFO {which, wave, rate_or_div, restart}` (global LFO op is engine-scoped; still fits INST_OP's envelope with slot ignored, or ride `MSG_CONTROL_CHANGE` for rate/depth as today — decide at implementation, both are wired paths).
- Live sources: `SRC_MODWHEEL`/`SRC_AFTERTOUCH` need CC1/pressure forwarded — extend the ESP32 MIDI task to forward CC1 + channel pressure as `MSG_CONTROL_CHANGE{param=PARAM_MACRO-adjacent internal ids}`… cleaner: add `MSG_MIDI_CC {cc, value, channel}` (0x56) so the Daisy owns the CC→source map. Round-trip test + dispatch test same commit.
- P-lock edit/record ops are `SEQ_PATTERN_OP` extensions (already reserved in `sequencer.md` §4).

## 7. UI (ESP32)

1. **Step hold + knob** = write p-lock (the core Elektron gesture); locked steps render with a corner badge; step hold shows current locks with per-lock clear.
2. **Mod page** (per instrument slot): 8 slot rows `source → dest, depth, curve`; encoder-driven; live value bars per source (needs a coalesced `MSG_INST_STATUS` extension or piggyback on meter cadence — 10 Hz is plenty).
3. **LFO page**: two global + voice LFO panels, tempo-sync toggle.

## 8. Test plan

- Host: `ApplyParamLocks` mapping tests per param id (value scaling, clamps); matrix evaluation golden tests (sources synthetic, verify per-block dest values incl. curves and unipolar/bipolar remap); env2/LFO block-rate progression; per-trigger random reproducibility with seeded RNG (`sequencer.md` §6 pattern).
- Host: revert-at-step-boundary for track-scoped locks (the "lock lasts one step" invariant).
- Hardware: DWT-measure control-tick cost with 8 voices × 8 slots + 3 LFOs active; budget < 10% of the tick. Audible: vibrato smoothness at block-rate pitch mod (sanity listen), filter-env sweep on the Stage A analog path via SRC_PARA_ENV → PARAM_ANALOG_CUTOFF.

## 9. Implementation status

- Voice-scoped lock application, individual-field live-edit protection, four-slot
  touch editing, single-slot clearing and pattern save/load are implemented.
  Host tests cover mapping, region bounds, unsupported ids, live-note isolation,
  slot identity and duplicate rejection. The two-board editor/recall HIL passed.
  Callback measurements are recorded separately in
  [callback-performance-log.md](../callback-performance-log.md).
- The block modulation surface and per-trigger velocity/note/random sources are
  implemented. Env 2 is a block-rate source and shares note/release/choke
  lifecycle with Env 1. Its editable Instrument parameters follow in Phase 2.5.
- The eight-row Instrument matrix and Instrument Mod editor are implemented.
  Foreground edits publish complete per-Track snapshots through mailboxes;
  the callback never reads a partially edited matrix.
- The engine currently ticks two global LFOs. The target above replaces the
  second global source with two Instrument-owned per-voice LFOs; that migration,
  Env 3 and expanded destinations remain Phase 2.5 work.
- MIDI CC/channel-pressure source wiring, live lock recording, global LFO
  editing and analog/group lock lifetimes remain open. The corresponding
  gestures and protocol extensions above describe targets, not current controls.
