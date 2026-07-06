# Sampling & Recording — Design

**Status**: Target design (unimplemented). Phase 2.5 in `roadmap.md`. Replaces the deleted legacy `Sampler` (removed 2026-07-05, code review C2 — it was disconnected end-to-end; its one lesson is preserved here: **preallocate, never grow, in the record path**).
**Dependencies**: voice manager (done — auditioning), `instrument-model.md` (assign-to-zone), SD write path (exists), `offline-sample-editing.md` (destructive trim/normalize later; **not** required for v1).
**Lineage**: the Emax sampling workflow — arm with a threshold, watch the VU, capture with pre-roll, audition immediately, truncate, place on the keyboard — closed the loop in seconds. That loop, not any single DSP feature, is the target.

---

## 1. Signal path and contexts

Input: **Daisy SAI1 codec stereo in** (as-built, currently discarded by `Callback()` — roadmap Phase 1 item 3 noted this).

```
Callback (1 ms, RT):    in[] → int16 convert → SPSC record ring (staged pre-roll + capture)
                        in[] × monitor_gain → mix into out[] (input monitoring, toggle)
                        input peak/RMS accumulation (for REC_STATUS meters)
Main loop:              drain ring → preallocated SDRAM extent (capture buffer)
                        on stop: chunked WAV write SDRAM → SD (render-scheduler budget rules)
```

Rules honored: no allocation/IO in the callback (ring is a fixed `DMA`-free static buffer, int16 conversion is per-sample arithmetic); SD writes are main-loop, chunked, ≤ 2 ms per step (`offline-sample-editing.md` §3 budget); the capture extent comes from `SampleMemMgr::alloc` **at arm time**, never during capture.

## 2. Record engine (Daisy)

Module: `firmware/daisy/src/audio/recorder.hpp` — HAL-free core (state machine + ring/extent bookkeeping over caller-supplied buffers), host-testable; thin glue in `audio_engine.cpp`.

```cpp
enum class RecState : uint8_t { Idle, Armed, Capturing, Stopping, Saving };

struct RecConfig {
    uint8_t  source;          // 0 = codec stereo in, 1 = L mono, 2 = R mono,
                              // 3 = resample (record the master mix output — free bounce path)
    uint8_t  channels;        // 1|2, derived from source
    uint16_t threshold;       // 0 = manual start; else q15 magnitude that trips capture
    uint16_t preroll_ms;      // 0..500, kept while Armed so the transient survives (Emax fix)
    uint32_t max_frames;      // capture ceiling; extent = max_frames·channels·2 B, alloc at arm
    uint8_t  monitor;         // input→output monitor on/off
};
```

- **Armed**: callback writes into a circular pre-roll region (preroll_ms worth); threshold trip (|sample| ≥ threshold on either channel) → Capturing, pre-roll contents become the first frames. `threshold == 0` waits for an explicit start command.
- **Capturing**: ring → extent via main-loop drain. Hitting `max_frames` auto-stops (state → Stopping) — never overruns the extent (the `Sampler` lesson).
- **Stopping → Saving**: take exists in SDRAM; it is *immediately auditionable* — register it in the slot sample table as a transient entry (`sample_id 0xFFxx` range) so `VoiceManager` can play it before any SD write. Saving writes `0:/wavex/recordings/take_NNN.wav` (16-bit PCM, engine rate, N from a persisted counter) + a `.wxs` sidecar with start/end markers preset to a **non-destructive auto-trim**: first/last frames exceeding −60 dBFS, padded 10 ms — the Emax "truncate" as markers, zero DSP, reversible. Destructive truncate/normalize arrive with Phase 4 render ops.
- **Resample source** (`source == 3`): taps the post-mix stereo out instead of `in[]` — free internal bounce (sample your own sequenced pattern; the Emulator crowd's favorite trick). Same path otherwise.

Sizing: 60 s stereo @48 kHz int16 = 11.5 MB — fine against 64 MB SDRAM; default `max_frames` = 30 s, UI-settable to 120 s mono.

## 3. Protocol (reserved block 0x70–0x77)

| Type | ID | Dir | Payload | Purpose |
|---|---|---|---|---|
| MSG_REC_CTRL | 0x70 | E→D | `RecCtrlMessage{cmd, RecConfig}` | cmd: ARM, DISARM, START (manual trip), STOP, SAVE{name?}, DISCARD, AUDITION{note} |
| MSG_REC_STATUS | 0x71 | D→E | `RecStatusMessage{state, frames_captured, frames_max, rms_l, rms_r, peak_l, peak_r, clip_count, error}` | pushed 15 Hz while non-Idle (meters ride here, not MSG_METER_PUSH, which stays output-scoped) |

Post-save handoff: SAVE's completion `REC_STATUS{state=Idle, error=0}` carries the final path in a follow-up `MSG_SAMPLE_STATUS`-style message — simplest: add `char path[96]` to `RecStatusMessage` (fits the 256 class), populated only on save completion. UI then offers **"assign to zone"** which is just `INST_OP_SET_ZONE_SAMPLE` (`instrument-model.md` §6) — recording ends where the instrument model begins, closing the Emax loop.

## 4. UI (ESP32) — one page, `ui_sample_record_page.cpp` exists as a stub host

1. **Meter block**: input VU (RMS + peak-hold + clip counter) live from REC_STATUS while armed — set gain *before* committing, the whole point of the Emax VU screen.
2. **Config row**: source, threshold (with "manual" at zero), pre-roll, max length, monitor toggle.
3. **Transport row**: Arm → (threshold trip or Start) → Stop → [Audition | Save | Discard]. Audition fires `REC_CTRL{AUDITION, note=60}` — Daisy triggers the transient take through a scratch voice.
4. **After save**: waveform preview (existing `MSG_PREVIEW_REQ` path works on the saved file), auto-trim markers shown/adjustable (sidecar edit), "Assign to zone…" picker.

## 5. Failure modes (design them in, don't discover them)

- Extent alloc fails at arm → REC_STATUS error immediately; UI shows free sample RAM (already available via `SampleMemStatusMessage`).
- SD write fails mid-save → take remains in SDRAM, state returns to Stopping with error; user can retry save or discard. Temp+rename means no torn files ever.
- Reboot during capture → take lost (SDRAM), no SD corruption possible.
- Sequencer playing while recording: fully supported (that's the resample use case); the record drain shares the main loop with `PumpWavIO` — **priority: PumpWavIO first**, record drain second, save-chunks third (record ring must be sized for worst-case drain latency: 64 KB ≈ 340 ms stereo — generous).

## 6. Test plan

- Host: state machine (arm/threshold/pre-roll splice/max-frames stop/discard); threshold trip exactness (first out-of-threshold frame captured, pre-roll frames precede it in order); ring overflow behavior (drop-oldest + counter, never write past extent).
- Host: WAV writer golden bytes (header, odd frame counts); sidecar auto-trim marker math on synthetic fade-in/out material.
- Round-trip + dispatch tests for 0x70/0x71.
- Hardware: record from line-in while an 8-voice pattern plays — zero underruns during capture and during save; audition-before-save latency < 100 ms; clip counter sanity against a deliberately hot signal.

## 7. Implementation stages (one verified commit each)

1. `recorder.hpp` core + host tests (no wiring).
2. Callback/main-loop wiring (`in[]` finally consumed; monitor mix; meters) — bench-verify passthrough + meters before any capture UI exists.
3. Protocol 0x70/0x71 + round-trip + dispatch tests + doc row updates.
4. WAV save path + sidecar auto-trim + transient-take audition.
5. ESP32 record page; end-to-end bench: the full Emax loop (arm → play → capture → audition → save → assign → sequence) in one sitting.
