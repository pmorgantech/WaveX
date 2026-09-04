# MIDI Clock Sync & Tempo Follower — Design

**Status**: Target design (unimplemented). Required for the Phase 2 gate ("MIDI-clock-synced to a DAW without audible drift over 10 minutes"). Expands `sequencer.md` §1–2.
**Dependencies**: sequencer engine clocking core and the built ESP32 MIDI input path.
**Placement**: MIDI real-time bytes arrive on the ESP32 (DIN UART2 / USB), are timestamped at ingest, and forwarded over the UART link. The Daisy runs the tempo follower and is always the sequencer's timing authority.

---

## 1. The numbers (get these right — the whole design hangs off them)

- MIDI clock = **24 PPQN**. Tick period at BPM *b*: `T_us = 60e6 / (24·b) = 2.5e6 / b`.
  - 120 BPM → 20 833 µs (~20.8 ms), 48 ticks/s. 60 BPM → 41 667 µs. 300 BPM → 8 333 µs.
- BPM from measured spacing: `b = 2.5e6 / T_us`.
- Sequencer internal resolution is **96 PPQN** (`sequencer.md` §2) → exactly **4 internal ticks per MIDI clock**.
- Tempo advance per 1 kHz control tick, 32.32 fixed point: `dphase = 96·b / 60000` internal ticks per ms (0.192 at 120 BPM). Accumulated in 64-bit; never floats (drift rule from `sequencer.md`).

Link load: 48 msg/s at 120 BPM in 32 B packets ≈ 1.5 KB/s + framing — negligible at 2 Mbaud.

## 2. The clock-domain problem (why the naive design fails)

The ESP32's `esp_timer_get_time()` and the Daisy's audio-frame counter are **different clock domains** with independent crystal error (±tens of ppm) and no shared epoch. An ESP32 timestamp is meaningless as an absolute time on the Daisy. What each domain is good for:

- **ESP32 ingest timestamps** (µs, taken in the UART event handler / USB callback, *before* any batching): differences between consecutive timestamps are a low-jitter measurement of the **master's tick period** — jitter sources upstream of them are only DIN wire time (320 µs/byte at 31 250 baud, constant) and ISR-to-task latency (~100 µs class).
- **Daisy arrival times** (audio-frame counter captured when the link message is dispatched): jittery (link TX queue, main-loop cadence — ms class) but in the **same domain as the audio stream**, which is the only domain phase ultimately matters in.

**Design consequence**: estimate *period* from ESP32 timestamp deltas (precise), and servo *phase* from Daisy arrival times (noisy but unbiased — constant link latency shifts phase by a constant, which the musician cancels when nudging start alignment, and which stays constant so it never causes drift). Never mix the domains in one subtraction.

## 3. Protocol

One message, reserved ID from the 0x50 block:

| Type | ID | Dir | Payload |
|---|---|---|---|
| MSG_MIDI_CLOCK_EVENT | 0x55 | E→D | `MidiClockEventMessage` |

```cpp
struct MidiClockEventMessage {            // packed, named ctor per protocol.h conventions
    uint8_t  event;        // 0=CLOCK(0xF8) 1=START(0xFA) 2=CONTINUE(0xFB) 3=STOP(0xFC) 4=SPP(0xF2)
    uint8_t  source;       // 0=DIN, 1=USB (Daisy follows one source at a time; first-active wins)
    uint16_t tick_seq;     // wraps; gap detection for dropped CLOCK messages
    uint32_t esp_delta_us; // µs since the PREVIOUS event from this source (0 on first/START)
    uint16_t spp_beats16;  // SPP payload: MIDI beats (16th notes), event==SPP only
    uint16_t reserved;
};
```

Sending deltas instead of absolute ESP32 timestamps bakes the clock-domain rule into the wire contract — the Daisy *cannot* misuse an absolute foreign timestamp because it never receives one. ESP32 rules: timestamp in the ingest context (per-byte RX path of `midi_task.cpp` / tinyusb callback in `usb_midi_task.cpp`), enqueue immediately, never coalesce CLOCK events.

`MSG_SEQ_TRANSPORT` (0x50, `sequencer.md` §4) gains `clock_source` (0=internal, 1=MIDI) and `MSG_SEQ_PLAYHEAD` (0x53) gains `sync_state` (0=internal, 1=acquiring, 2=locked, 3=freewheel) + `measured_bpm_x100` for the UI.

## 4. Tempo follower (Daisy, control-tick context)

Module: `firmware/daisy/src/sequencer/tempo_follower.hpp` — HAL-free, host-testable: inputs are `(event, esp_delta_us, daisy_frame_now)` tuples, output is the 32.32 phase increment + state.

### 4.1 Period estimator

- Keep the last 5 CLOCK deltas (`esp_delta_us`); take the **median** (kills single outliers from USB batching or a dropped-then-doubled delivery), then EMA with α = 1/8 into `period_us`.
- Reject deltas outside ±25% of current `period_us` once locked (they update a "tempo-jump detector" instead: 3 consecutive rejected-but-mutually-consistent deltas ⇒ hard re-acquire at the new tempo — handles a DAW tempo change without wallowing through the EMA).
- `tick_seq` gaps: a missing CLOCK means the next delta covers *n* periods; divide by the gap count before feeding the estimator.

### 4.2 Phase servo

- The sequencer's master phase `Φ` (internal 96-PPQN ticks, 32.32) normally advances by `dphase(period_us)` per control tick.
- Each CLOCK event *k* should land at `Φ_expected = 4k` (+ start offset from START/SPP). Measure `err = Φ_at_arrival − 4k` using the Daisy arrival tick, smoothed with a 1-pole (α = 1/8, ≈150 ms at 48 ticks/s).
- Correct by **slewing the rate, never stepping the phase**: `dphase_effective = dphase · (1 + clamp(−Kp·err, ±0.005))` — a ±0.5% rate trim erases 1 ms of phase error in ~200 ms without any audible lurch. Start with `Kp = 0.02` per internal tick of error; tune in the host jitter harness, not on stage.
- Because the trim also absorbs crystal-ppm mismatch between the domains, there is no separate drift term.

### 4.3 State machine

```
INTERNAL ──(transport clock_source=MIDI)──► ACQUIRING
ACQUIRING ──(5 consistent deltas, ±2%)────► LOCKED
LOCKED ────(no CLOCK for 2× period)───────► FREEWHEEL   (keep playing at last rate)
FREEWHEEL ─(CLOCK resumes)────────────────► ACQUIRING → LOCKED (phase re-anchored to next CLOCK)
FREEWHEEL ─(STOP received / user stop)────► stopped
any ───────(clock_source=internal)────────► INTERNAL
```

Freewheel is deliberate (Elektron behavior): a flaky cable pauses sync, not the music. START resets `Φ` to 0 effective at the *next* CLOCK (MIDI spec: F8 after FA marks the downbeat). CONTINUE resumes at the SPP-derived position: `Φ = spp_beats16 · 24` internal ticks.

## 5. MIDI clock OUT (Daisy → world)

The Daisy is the timing master; the ESP32 is a dumb serializer:

- Sequencer emits a compact `MSG_SEQ_CLOCK_OUT` (0x57, D→E) `{uint8_t event; uint16_t tick_seq;}` at each 24-PPQN boundary (plus START/STOP/CONTINUE/SPP on transport changes); ESP32 writes 0xF8/0xFA/… to DIN UART and USB immediately on receipt, bypassing any TX coalescing.
- Expected jitter = link + task latency, ~1–2 ms class. `sequencer.md` §1 already flags the fallback: if measured jitter exceeds ~1 ms and it matters musically, move DIN out to a spare Daisy UART pin (decision point, bench-measured, Phase 2 gate). The message design above is transport-agnostic so the fallback doesn't change the sequencer.

## 6. Test plan

Host (`firmware/daisy/tests`, follower is HAL-free):

1. **Clean lock**: 120 BPM synthetic stream → LOCKED within 5 ticks; `measured_bpm` within ±0.1.
2. **Jitter**: ±2 ms uniform jitter on Daisy arrivals + ±200 µs on deltas → phase error RMS < 0.5 internal tick; no state flapping over 10 000 ticks.
3. **Drift**: master at 120.000, follower domain clock scaled by +50 ppm → zero accumulated beat drift over a simulated hour (the rate trim absorbs it).
4. **Tempo jump**: 120→140 step → re-lock < 1 bar; ramp 120→140 over 8 bars → tracks within ±1 BPM.
5. **Dropout**: 300 ms clock gap → FREEWHEEL, playback phase continues advancing monotonically; resume → re-lock without phase step > 1 internal tick.
6. **Transport**: START/CONTINUE+SPP position math golden tests.

Hardware (Phase 2 gate): DAW at 120 BPM, 10-minute recording of WaveX audio against the DAW's own metronome — beat alignment drift < ±3 ms over the run; audible check for tempo "breathing" during deliberate cable-wiggle dropouts.

## 7. Implementation stages (one verified commit each)

1. `tempo_follower.hpp` + full host-test suite above (no protocol yet).
2. `MidiClockEventMessage` + `MSG_SEQ_CLOCK_OUT` structs, round-trip tests, `inter-mcu-protocol.md` rows.
3. ESP32 ingest: timestamp + forward path in `midi_task.cpp` / `usb_midi_task.cpp` (real-time bytes currently parsed-and-dropped by `midi_stream_parser` interleave handling — tap them there).
4. Daisy wiring: dispatcher route (extend `message_dispatch_test.cpp` — the C1 lesson: a dispatch-level test per routed type, so a silent stub can't recur), follower feeding the sequencer phase.
5. Clock out + ESP32 serializer; bench jitter measurement recorded in this doc.
