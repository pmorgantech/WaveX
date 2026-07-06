# Scenes & Performance Macros — Design

**Status**: Target design (unimplemented). Phase 5 in `roadmap.md` (macros were already named there; scenes join them). Nothing here blocks earlier phases, but the **param slew engine** (§3) is small and worth landing with Phase 2.5's modulation work since both touch the same control-tick surface.
**Dependencies**: `param-locks-and-modulation.md` (macros are mod sources; scenes snapshot the same param id space), sequencer (pattern select in scenes), mixer (`output-routing-and-mixer.md`) for track levels in scenes.
**Lineage**: E-mu's realtime-control assignments (one physical controller → many routed destinations with per-route range) are exactly the macro model; scenes are the modern live-set layer on top.

---

## 1. Macros (4, engine-side sources)

A macro is a **named 0..1 value** broadcast as `MSG_CONTROL_CHANGE{param = PARAM_MACRO_n}` (ids reserved in `param-locks-and-modulation.md` §1). The Daisy holds the live value; routing happens through mod-matrix slots (`SRC_MACRO_n → dest, depth, curve`) — macros get the matrix's curves and scaling for free, no second routing system.

- **Assignment UX** (ESP32): "learn" flow — touch a macro, wiggle a destination knob, set depth/range; under the hood this writes a mod slot via `INST_OP_SET_MOD_SLOT`. Macro→destination maps are instrument-scoped (stored in `.wxi`), matching E-mu semantics (a preset carries its realtime-control routing).
- **Physical control**: one encoder bank page maps encoders 1–4 to macros; MIDI CC in (via `MSG_MIDI_CC`, 0x56) can be bound to a macro (CC-learn table, ESP-side, persisted in project settings).

## 2. Scenes (8 slots, performance snapshots)

A scene captures **performance state, not content**: it must be safe to punch mid-set.

```
Scene := { pattern_id,                       // optional: 0xFF = leave playing pattern alone
           macro_values[4],
           track_mix[16]{gain, mute},        // mixer doc
           param_overrides[≤16]{param_id, slot, value},   // sparse CC-level settings
           morph_ms }                        // recall ramp time, 0..2000
```

Not captured: instruments, zones, patterns' contents, samples — scenes reference, never contain (recalling a scene can't trigger SD I/O beyond nothing; everything applied is RAM-resident engine state).

- **Storage**: ESP32 owns scene definitions (project-scoped, WXCF chunk in the project file per `instrument-model.md` §5 container; file writes on explicit save, not on every tweak). The Daisy is stateless about scenes — recall is a burst of ordinary messages plus §3's ramp header, so link-loss recovery rules stay simple.
- **Recall path**: ESP32 sends `MSG_SCENE_APPLY` (0x7A, E→D): `{morph_ms; count; entries[≤24]{param_id, slot, value}}` (fits the 128 B class) — one message, atomically applied at a control-tick boundary, each param ramped per §3. Pattern change (if any) goes separately through the normal `SEQ_PATTERN_OP` quantized-to-bar switch (sequencer's rule, unchanged).
- **Gestures**: tap = recall (with morph), hold = save-current-into-slot, hold+turn = adjust morph time. Row of 8 chips on the perform page; TCA8418 top row mirrors when available.

## 3. Param slew engine (Daisy, control tick)

Small generic mechanism, used by scene morphs and reusable anywhere a param step would zipper:

- Table of ≤ 32 active ramps `{param_id, slot, current, target, step_per_tick}`; each control tick advances and applies through the **same param-apply function live CCs use** (single write path — no scene-only application code to drift out of sync).
- `morph_ms = 0` applies immediately. Ramps on the analog CV params compose naturally: they feed the staged CV values the MCP4728/MCP48 flush already consumes.
- Collision rule: a new ramp for `(param_id, slot)` replaces the old one, starting from `current` (no jumps).

## 4. Tests

- Host: slew table (ramp math, collision restart, zero-morph, full 32-ramp load in one tick under budget); scene-apply ordering (all entries land on one tick boundary — no half-applied scene observable between ticks).
- Round-trip + dispatch tests for 0x7A; WXCF scene chunk round-trip.
- Hardware: A/B two scenes with 2 s morph while an 8-voice pattern plays — no zipper noise on digital params, CV morph visible on scope as a clean ramp; recall latency (tap → first param moving) < 30 ms.

## 5. Implementation stages (one verified commit each)

1. Slew engine + host tests (lands well alongside Phase 2.5 modulation work).
2. `MSG_SCENE_APPLY` + round-trip/dispatch tests + doc rows.
3. ESP32 scene store (WXCF project chunk) + recall burst + perform-page chips.
4. Macro learn flow + CC-learn binding table.
5. Bench morph verification (scope on CV, ears on digital).
