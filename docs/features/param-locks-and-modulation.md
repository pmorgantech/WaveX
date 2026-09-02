# Parameter Locks & Modulation Matrix — Design

**Status**: Partially built (2026-09-02) — see §9 for exact stage status. The mod matrix, its two primitives (LFO, param slew) and the control-tick wiring into the audio callback exist and are host-tested; it is a no-op on hardware today because nothing yet populates a slot. P-locks (§2) have not been started. P-locks are Phase 2 (already named in `sequencer.md` §3); the modulation matrix and LFOs are Phase 2.5.
**Dependencies**: sequencer step scheduler (Phase 2), `instrument-model.md` (matrix slots are instrument-scoped), voice manager (done).
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
    SRC_ENV_FILTER,                            // per-voice 2nd envelope (§4)
    SRC_LFO1, SRC_LFO2,                        // global LFOs (§5)
    SRC_LFO_VOICE,                             // per-voice LFO (§5)
    SRC_RANDOM,                                // per-trigger S&H, seeded RNG
    SRC_MACRO_1, SRC_MACRO_2, SRC_MACRO_3, SRC_MACRO_4,
    SRC_MODWHEEL, SRC_AFTERTOUCH,              // from MIDI CC1 / channel pressure
    SRC_PARA_ENV,                              // Stage A paraphonic envelope (analog dests)
};
```

**Evaluation model — control-rate, never per-sample**: once per 1 ms control tick, for each active voice, `mod[dest] += depth · curve(source_value)` over its instrument's slots. Cost ceiling: 8 slots × 8 voices × ~10 ops = trivial. Per-trigger sources (velocity, note, random) are sampled into the voice at trigger and treated as constants. Destinations applied at block rate: cutoff → `filter.SetCutoff` once per block (one-pole recomputes its coefficient — cheap), gain/pan → block-constant multipliers, pitch → `increment` multiplier update once per block (this quantizes vibrato to 1 kHz steps, which is inaudible; do **not** move pitch mod per-sample without a DWT budget check).

This requires the modest `Voice` surface: `SetBlockModulation(cutoff_mul, gain_mul, pitch_mul, pan_offset)` applied at the top of its render slice — one struct write, callback-safe.

## 4. Second envelope (filter envelope)

E-mu voices had filter + amp envelopes; WaveX `Voice` has one (amp). Add `Envelope env2` to `Voice` (same linear `envelope.hpp` class, retrigger/release alongside env1), exposed only as `SRC_ENV_FILTER`. Zone carries its ADSR (defaults in `instrument-model.md` wire-Zone chunk v2 — append fields, bump chunk version). Cost: one more `Process()` per active voice per sample… **no** — env2 is a *mod source*, so it's evaluated **once per block** (32.32 phase advanced by block length), not per sample. Keeps the callback budget flat.

## 5. LFOs

- **2 global LFOs** (control-tick, in `AudioEngine`): sine/tri/saw/square/S&H, rate either Hz (0.02–20) or tempo-synced divisions (1/16 … 4 bars — needs the sequencer clock; free-runs in Hz until Phase 2 lands). Phase-restart options: free, on-transport-start, on-any-note.
- **1 per-voice LFO**: phase accumulator per voice, waveform/rate from the instrument, evaluated per block. Retriggers at note-on with optional `delay_s` and `fade_s` (the classic E-mu delayed-vibrato envelope on the LFO — one ramp, two params).
- State: global LFO params live per-instrument? **No** — global LFOs are engine-global (like a modular's LFO bank); per-voice LFO params are instrument-scoped. This keeps multi-timbral behavior sane: slot 3's wobble doesn't change because slot 5 loaded a new instrument.

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
2. ~~`Voice` block-modulation surface~~ + env2 + per-voice LFO, host-tested (extends `VoiceManagerTest`). **Block-modulation surface done**: `Voice::SetBlockModulation()`, plus the per-trigger sources (`SRC_VELOCITY`/`SRC_NOTE`/`SRC_RANDOM`) sampled once at `Trigger()`. **env2 and the per-voice LFO are not built** — `SRC_ENV_FILTER`/`SRC_LFO_VOICE` evaluate to 0 in the meantime, the same treatment `SRC_PARA_ENV` already gets.
3. ~~Global LFOs~~ + matrix evaluator in the control tick, host-tested; DWT numbers recorded on bench. **Done, on host**: `VoiceManager::TickModulation()` runs once per callback (audio_engine.cpp's `Callback()` — one callback IS one 1kHz control tick), ticks two engine-global `Lfo` instances into `SRC_LFO1`/`SRC_LFO2`, and evaluates `EvaluateModMatrix()` per sounding voice. The instrument-scoped slot storage this evaluator reads from does not exist yet (`Instrument` has no `ModSlot` array) — `s_mod_slots` is a fixed engine-global array nothing currently writes, so this is a no-op on hardware until stage 4's protocol op lands. **DWT numbers not recorded** — no hardware bench session yet.
4. Protocol ops (INST_OP extensions, MSG_MIDI_CC 0x56) + round-trip + dispatch tests + `inter-mcu-protocol.md`, same commit; retire the 0x0A alias. Also needs instrument-scoped `ModSlot` storage (`Instrument` has none today - stage 3 reads from a temporary engine-global array instead).
5. UI: step-hold p-lock gesture + mod/LFO pages.
6. Live p-lock recording (motion capture window logic host-tested first).
