# Sequencer / Groovebox Engine — Design

**Status**: Core scheduler/protocol built and host-tested — `pattern.hpp`, `sequencer_scheduler.hpp`, `tempo_follower.hpp`, `sequencer_transport.hpp` (all HAL-free, ~60 host tests), plus the 0x50–0x57 protocol messages with round-trip and dispatch tests. Callback integration (turning scheduled events into audible voice triggers), MIDI clock out, the pad/step-editor UI, and persistence are open — see `roadmap.md` Phase 2 and `features/digital-voice-audition.md` stages 5–8. This is still the defining groovebox feature and the largest gap between the product vision and what's audible.

## 1. Placement: the sequencer engine lives on the Daisy

Timing is the whole game. The engine that decides *when* a voice triggers must be phase-locked to the audio stream, and the only clock with that property is the audio callback. The ESP32 UI is an *editor and remote control* for the sequencer; it never generates trigger timing.

Consequences:

- Pattern data (the playable representation) is resident in Daisy RAM; the UI edits it via protocol ops and receives playhead feedback.
- MIDI clock **out** is generated on… the ESP32 (owns MIDI I/O) from tick events sent by the Daisy — acceptable because MIDI clock granularity (24 PPQN ≈ 10–20 ms) is coarse relative to link latency, but jitter must be measured; if it exceeds ~1 ms, move MIDI DIN out to the Daisy directly (spare UART pins exist).
- MIDI clock **in** (slave sync) arrives on the ESP32, is timestamped, and forwarded; the Daisy runs a PLL-style tempo follower so link jitter doesn't modulate the groove.

## 2. Clocking model

- Master timebase: the 1 kHz control tick (= audio block boundary), with **intra-block sample offsets** for trigger accuracy. A step scheduled at sample 17 of block N starts rendering at exactly that frame — voices accept a start-offset within block. This gives sample-accurate sequencing with a 1 ms scheduling quantum.
- Tempo math in fixed point: ticks-per-step derived from BPM × PPQN (use 96 PPQN internally for micro-timing/swing resolution), accumulated in 32.32 to avoid drift.
- Swing = per-step timing offset table; micro-timing = signed per-step offset in PPQN ticks; both fold into the same scheduler.

## 3. Data model (v1)

```
Project
├── Kits[≤16]            # pad → sample ref (path + sidecar markers) + voice params
├── Patterns[≤128]
│   ├── length (1–64 steps), scale (1/16, 1/32, triplet…), swing
│   └── Tracks[≤16]      # one per pad/voice
│       └── Steps[64]: {on, velocity, probability, micro_offset,
│                        retrig(count,rate), param_locks[≤4]{param_id, value}}
├── Songs[≤16]: ordered (pattern, repeats) list
└── Tempo, master params
```

- **Vocabulary and ownership, confirmed 2026-09-02, "Patch" → "Instrument" 2026-09-03** (`track-and-patch-model.md` §1/§3.5): a pattern's row *t* plays through **Track** *t*'s **Instrument** — "Kits" above are drum-mode Instruments loaded into Tracks, referenced by path (or recalled from the Project's Bank), not a separate list. **Tempo moves to the Song** (project keeps a default for pattern mode); swing stays pattern-level with a Song default a pattern can follow; the default pattern length becomes **32** (2 bars of 16ths — `pattern.hpp` defaults to 16 today). `pattern.hpp`'s inner `Track` struct is to be renamed `TrackSteps` so "Track" means one thing.
- **Param locks** (per-step parameter overrides, Elektron-style) reuse the existing `ControlParameter` ids — application semantics (trigger-param overrides, one-step lifetime for track-scoped ids) are pinned in `param-locks-and-modulation.md` §2.
- **Kits are drum-mode instruments** (decision 2026-07-05): the kit structure above is the drum-mode subset of `instrument-model.md`'s zone model (pad *p* = zone with `key_lo == key_hi`), and `KIT_OP` is subsumed by `MSG_INST_OP` (0x54 stays reserved-unused). Melodic track types extend this pattern model in `melodic-sequencing.md`.
- **Choke groups** live in the kit (e.g. open/closed hat), enforced by the voice manager (`VoiceManager::Choke`, `instrument-model.md` §3).
- Serialization: versioned binary chunks on SD (`project.wxp`), written atomically (temp + rename). Design the format doc before code; include format version + per-chunk lengths so old firmware can skip unknown chunks.

## 4. Protocol extensions (design + round-trip tests before UI work)

| Message | Direction | Purpose |
|---|---|---|
| SEQ_TRANSPORT | E→D | play/stop/continue, tempo set, song position |
| SEQ_PATTERN_OP | E→D | step toggle/edit, track mute, pattern select, length/scale/swing — small idempotent ops, not bulk uploads |
| SEQ_PATTERN_SYNC | both | bulk pattern read/write for project load/save (chunked, size-class 1024/2048) |
| SEQ_PLAYHEAD | D→E | current pattern/step/beat, coalesced to ≤ 30 Hz for UI/LED feedback |
| KIT_OP | E→D | pad→sample assignment, choke groups, kit params |

Edits are applied between steps (double-buffered pattern rows) so editing while playing never tears a step.

## 5. UI surfaces (ESP32)

1. **Pad page**: 4×4 grid (TCA8418 matrix + touch), velocity via touch position or fixed levels, kit select, mute mode. TLC5947 LEDs mirror step/playhead state.
2. **Step editor**: track lanes, step toggles, hold-step-turn-encoder for param locks, page switching for >16-step patterns.
3. **Pattern/song page**: chain patterns, arrangement.
4. **Groove page**: swing, scale, humanize.

## 6. Test plan

- Host-testable scheduler core: pattern + tempo in → sorted (frame, event) stream out; golden tests for swing/micro-timing/probability (seeded RNG).
- Drift test: 10-minute render at 120 BPM must place beat 1200 exactly at frame 28,800,000 (±0).
- Sync test: slaved to MIDI clock with ±2 ms jitter injected, tempo follower stays within ±0.5 BPM and re-locks within 1 bar after a tempo jump.
- Link-loss test: UI disconnect mid-playback → pattern keeps playing; reconnect resyncs playhead display.
