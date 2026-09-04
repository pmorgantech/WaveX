# Parameter Locks & Modulation Matrix — Design

**Status**: Partially built (2026-09-02) — see §9 for exact stage status. The mod matrix, its two primitives (LFO, param slew), the control-tick wiring into the audio callback, the second envelope, and instrument-scoped `SET_MOD_SLOT` protocol wiring all exist and are tested (host + protocol round-trip/dispatch); the Daisy device build and ESP32 build are both green. It is a no-op on hardware today only because no UI yet sends `SET_MOD_SLOT` (stage 5). `SRC_MODWHEEL`/`SRC_AFTERTOUCH` still read 0 — MIDI CC/aftertouch forwarding from the ESP32 MIDI task is unbuilt. P-locks (§2) have not been started. P-locks are Phase 2 (already named in `sequencer.md` §3); the modulation matrix and LFOs are Phase 2.5.
**Dependencies**: sequencer step scheduler (Phase 2), `instrument-model.md` (matrix slots are instrument-scoped), voice manager (done). **Revised 2026-09-04**: the two-oscillator Instrument (`track-and-patch-model.md` §3.1) fixes the source/destination set this matrix serves — three envelopes, two per-voice LFOs, one global LFO, oscillator and wavetable-position destinations — appended to the enums below, never renumbered.
**Lineage**: two ancestries deliberately fused — Elektron parameter locks (per-step sound design) and the E-mu EIII **realtime controls matrix** (velocity/wheel/pedal → pitch, filter, level, LFO amount, attack — routed, not hardwired).

---

## 1. Parameter id space (one namespace for everything)

`ControlParameter` (`protocol.h`, currently 0x01–0x0A) becomes the single id space used by live CCs, p-locks, mod-matrix destinations, macros, and scenes. Extend (append-only):

```cpp
enum ControlParameter : uint8_t {
    // 0x01..0x0A existing (VOLUME, FILTER_CUTOFF, FILTER_RESONANCE, ENV A/D/S/R,
    //                      LFO_RATE, LFO_DEPTH, MODULATION_MATRIX*)
    PARAM_PITCH_OFFSET   = 0x0B,  // ± semitones ×100 (cents), voice-scoped
    PARAM_PAN            = 0x0C,
    PARAM_SAMPLE_START   = 0x0D,  // 0..65535 → 0..100% of region
    PARAM_LOOP_START     = 0x0E,
    PARAM_GAIN           = 0x0F,  // voice/track gain (distinct from master VOLUME)
    PARAM_MACRO_1        = 0x10,  // performance macros, scenes-and-performance.md
    PARAM_MACRO_2        = 0x11,
    PARAM_MACRO_3        = 0x12,
    PARAM_MACRO_4        = 0x13,
    PARAM_ANALOG_CUTOFF  = 0x14,  // Stage A shared / Stage B per-group CV cutoff base
    PARAM_ANALOG_RES     = 0x15,
};
// *PARAM_MODULATION_MATRIX (0x0A) is currently a temporary alias for env→cutoff
// depth (roadmap item 5 stage 3). When matrix slots land (§3), 0x0A is
// retired-but-reserved and the alias behavior is deleted in the same commit.
```

Values stay `uint16_t` on the wire (existing `ControlChangeMessage`); each param defines its own mapping (documented in a table in `protocol.h` comments, tested in the param-apply unit tests).

**Scoping rule**: a param write has a scope — *(a)* instrument-slot base value (live CC / UI knob), *(b)* per-trigger override (p-lock), *(c)* modulation offset (matrix, computed per block). Final per-voice value = `clamp(base + plock_override? : base, then + Σ modulation)`. P-locks **replace** the base for that trigger (Elektron semantics); modulation **adds**.

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

## 9. Implementation stages (one verified commit each)

1. Param id extension + `ApplyParamLocks` + scheduler application path, host-tested (pure functions, no protocol). **Not started** — only the `ParamLock` data model and its pass-through into `TriggerEvent` exist (`sequencer/pattern.hpp`, `sequencer_scheduler.hpp`); nothing applies a lock to a `VoiceTriggerParams`.
2. ~~`Voice` block-modulation surface~~ + ~~env2~~ + per-voice LFO, host-tested (extends `VoiceManagerTest`). **Block-modulation surface done**: `Voice::SetBlockModulation()`, plus the per-trigger sources (`SRC_VELOCITY`/`SRC_NOTE`/`SRC_RANDOM`) sampled once at `Trigger()`. **env2 done**: `Voice::env2`, released/choked alongside the amp envelope, advanced once per block via the new `Envelope::AdvanceBlock(n)` (§4's "not per sample" requirement, verified by pinning it against `n` calls to `Process()` across every stage-transition boundary). ADSR comes from new `filter_env_*` fields on `VoiceTriggerParams`, defaulted rather than zone-derived — Zone has no ADSR-for-SRC_ENV_FILTER fields yet, that's still the wire-Zone chunk v2 bump §4 names. **The per-voice LFO is not built** — `SRC_LFO_VOICE` still evaluates to 0, the same treatment `SRC_PARA_ENV` already gets.
3. ~~Global LFOs~~ + matrix evaluator in the control tick, host-tested; DWT numbers recorded on bench. **Done, on host**: `VoiceManager::TickModulation()` runs once per callback (audio_engine.cpp's `Callback()` — one callback IS one 1kHz control tick), ticks two engine-global `Lfo` instances into `SRC_LFO1`/`SRC_LFO2`, and evaluates `EvaluateModMatrix()` per sounding voice against **that voice's own instrument's slots** (`ModSlotResolver`, resolved via `Voice::slot` — stage 4 below gave this evaluator real per-instrument storage to read from instead of the original temporary engine-global array). **DWT numbers not recorded** — no hardware bench session yet.
4. ~~Instrument-scoped `ModSlot` storage~~ + ~~`INST_OP` extension (`SET_MOD_SLOT`)~~ + ~~retire the 0x0A alias~~ — **done**. `Instrument::mod_slots[8]` (`instrument.hpp`), written by `SfzLoader::SetModSlot()` from a new `INST_OP_SET_MOD_SLOT` op riding `MSG_INST_OP` (extends the existing `InstOpMessage` wire struct with `mod_slot_index`/`mod_source`/`mod_dest`/`mod_depth`/`mod_curve`/`mod_flags`, `path` unused for this op) — ESP32 send wrapper `inter_mcu_send_mod_slot()`, round-trip test (`message_types_test.cpp`) and dispatch test (`message_dispatch_test.cpp`), `inter-mcu-protocol.md` updated. `PARAM_MODULATION_MATRIX`'s (0x0A) `OnControlChange` case is deleted (enum value stays reserved, per the design). Read directly from the audio callback with no mailbox — a `ModSlot` is smaller than this architecture's atomic word, but every field a torn read could produce is still bounds-checked downstream (`ModSources::Get()`/`EvaluateModMatrix()`'s `default` cases), so the worst case is one harmless-or-bounded control tick, self-correcting the next; promote to a mailbox like `s_voice_live_pending` if a bench session ever finds it audible. **Still open**: `MSG_MIDI_CC` (0x56) itself was already fully wired before this stage (struct, dispatch, round-trip/dispatch tests) but two things around it are not: the ESP32 MIDI task still drops incoming CC/channel-pressure instead of forwarding them (`midi_task.cpp`'s `ControlChange` case is a no-op, and channel pressure isn't even a parsed event yet in the shared MIDI stream parser), and nothing on the Daisy feeds a received CC into `ModSources.modwheel`/`.aftertouch` — `SequencerTransport::OnMidiCc()` still only records `last_cc_*` for tests. `SRC_MODWHEEL`/`SRC_AFTERTOUCH` read 0 until that's built.
5. UI: step-hold p-lock gesture + mod/LFO pages.
6. Live p-lock recording (motion capture window logic host-tested first).
