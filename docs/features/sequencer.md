# Sequencer / Groovebox Engine — Design

**Status**: Target design (nothing implemented). Phase 2 in `roadmap.md`. This is the defining groovebox feature and currently the largest gap between the product vision and the code.

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

- **Param locks** (per-step parameter overrides, Elektron-style) reuse the existing `ControlParameter` ids — the step scheduler applies them through the same path as live control changes, so no new audio-engine surface is needed.
- **Choke groups** live in the kit (e.g. open/closed hat), enforced by the voice manager.
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
