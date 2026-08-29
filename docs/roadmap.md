# WaveX Implementation Roadmap

**Status**: Canonical implementation-order document. Read `architecture.md` first.
**Last updated**: 2026-08-29 (pruned completed items; added Phase 1.5)

Phases are ordered by dependency, not calendar. Within a phase, items are listed in recommended implementation order. Every phase ends with the test gate that must be green before moving on.

**This document lists work that is still open.** Completed work is recorded in [`CHANGELOG.md`](../CHANGELOG.md) and in git history — it is deliberately *not* kept here, so this file stays readable as a plan rather than becoming an archive. The one exception is [§ Outstanding hardware verification](#outstanding-hardware-verification): those items are code-complete but unproven, which makes them open work, not history.

> **Trust the code, not this file.** Rationale recorded here has been factually wrong more than once, including an item marked "Done" that was not. Before acting on a claim below that something is dead, low-risk, or finished, verify it against the source.

---

## Phase 0 — Foundation Hardening

### 0.1 Toolchain upgrades still open

| Component | Pinned today | Recommendation |
|---|---|---|
| CMSIS-DSP | 1.14.4 (vendored inside libDaisy `Drivers/CMSIS-DSP`) | Upgrade opportunistically to 1.17.0, not urgently. 1.15–1.17 fixes are mostly Helium/Neon (irrelevant on Cortex-M7) plus generic out-of-bounds fixes in biquad/FIR-interpolation — relevant once offline DSP uses those kernels. Bump the submodule pointer within our libDaisy fork; do not hand-copy sources (the upstream build layout changed). |
| ESP-IDF | 5.5.x | Stay on 5.5.x; track patch releases. Plan the 6.0 migration as a dedicated task. Three tripwires: (1) ESP32-P4 default chip revision becomes v3.0, so binaries won't boot on rev < 3.0 silicon without `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` — check the module's silicon rev first; (2) legacy drivers are removed (audit third-party components, `esp_tca8418` is fetched from git and may lag); (3) every managed component must declare 6.0 compatibility, and the waveshare BSP is the likely laggard. Budget a spike branch. |

### 0.2 Repo cleanup still open

1. **Split `daisy_spi_link.cpp`** along the boundaries in `archive/daisy_spi_link_splitup_plan.md` (transport / packet / message-processing / bridges). Opportunistic as other work touches those files, not a big-bang refactor.
2. **UART is the transport of record.** `WAVEX_SPI_LINK_ENABLED` is hard-coded `0`, so the SPI link is compiled out of every image and UART carries all traffic including browse pages and wave chunks. Consolidating onto SPI-only is real protocol work — extending `protocol.h` for the UART-only message types, rewiring every send call site, then hardware bring-up. Track separately if prioritized. See `architecture.md` §4.4.

**Gate**: `make all` + `make test` clean from scratch; SD soak test passes.

---

## Phase 1 — Solid Playback Core

Order of implementation:

1. **Streamed-voice concurrency**: 2 concurrent streams with prebuffer admission control. The existing WAV-streaming path (`s_wav`, ring buffer, prebuffer, SD buffer slots in `audio_engine.cpp`) is entirely singleton/global; generalizing it to N admission-controlled streams is a refactor of that subsystem, not an extension of `VoiceManager` (which handles only RAM-resident playback).
2. **Recording, rebuilt**: the old `Sampler` was deleted — nothing fed its input, nothing rendered its playback, and its wire commands were dispatcher stubs. Rebuild it voice/streaming-integrated when scheduled. The preallocation lesson stands: no `reserve()` in the audio path; take a fixed extent from the SDRAM allocator at setup.
3. **Raise SPI link clock**: replace the bring-up `PS_16` prescaler; verify on scope, measure error rate at each step. Target: browse a 500-entry directory in < 500 ms; waveform preview of a 3-min WAV in < 1 s. **Blocked** on the SPI link being compiled out (0.2 item 2).

**Gate**: 8-voice drum kit playable from MIDI with zero underruns for 1 hour; paraphonic analog path (Stage A) calibrated and audible; both output/CV flag configurations compile in CI; host tests cover voice allocation and the sample-load → status → UI flow.

---

## Phase 1.5 — Sample Edit Page & UI Interaction Model (added 2026-08-29)

Non-destructive marker editing. Sits here, not in Phase 4, because the engine already supports everything it needs — `Voice` carries `start_frame`, `end_frame`, `loop`, `loop_start` and `loop_end` (Phase 1) — so this is a UI and protocol gap, not new DSP. Destructive operations (trim, normalize, fades, render jobs) stay in Phase 4.

Current state: the page draws the wireframe layout and START/END/ZOOM move the *preview window* only. Nothing is sent to the backend, so nothing is audible or persistent.

### 1.5.1 Marker model and protocol

1. **Four independent markers**: start, end, loop start, loop end. The voice already has all four; the wire does not. Needs a message carrying them per slot, plus a status reply so the UI can read back what the engine actually applied (clamping matters: loop points must stay inside start/end, and the engine is the authority on that, not the UI).
2. **Audition must honour the markers.** Today `MSG_SAMPLE_PLAY_INDEX_REQ` has no range field, so Audition plays the whole file. Playback should start at `start`, stop at `end`, and loop between `loop_start` and `loop_end` while looping is enabled. This is the single highest-value item here — without it the markers are decorative.
3. **Gain**: wire the GAIN card. Non-destructive playback gain per sample, applied at trigger (`VoiceTriggerParams::gain_mul` already exists).
4. **Save / Save As**: persist markers and gain. Prefer a WXCF sidecar (`features/instrument-model.md` §5) over a new per-file format — this is the same data a zone carries, and duplicating it invites divergence. Save As implies a filename entry surface, which does not exist yet.
5. **Sample selection from the edit page.** Currently the page edits whatever the browser last loaded, with no way to change it. Either a picker, or make the edit page accept a sample argument and have the browser push it.

### 1.5.2 Interaction model

**Decided (2026-08-29): a Shift modifier reveals an alternate softkey row.** Built and global — `UIPage::getShiftedSoftkeys()`, `UINavigator::toggleShift()`, a SHIFT chip in the header, and `BUTTON_SHIFT` intercepted in `InputDispatcher` so no page can swallow it. Three properties worth keeping:

- **Latched, not held.** Hold-and-press is awkward one-handed on a touch panel, and holding a key while turning the encoder is worse.
- **Sticky**: clears after one shifted key fires, and on navigation. A plain toggle gets left on and the next press does the wrong thing.
- **Inert, not hidden, on pages with no alternate row.** A control that appears and disappears as you navigate is harder to learn than one that is always there and sometimes dim.

Physical Shift still needs a key: `tca8418_keypad.cpp` maps keycode 4 → `BUTTON_SHIFT`, but the matrix mapping is a three-key stub and `WAVEX_ESP_BUTTON_MATRIX_ENABLED` gates it. Until then the header chip is the only way in.

Specific items:

1. **Draggable handles.** S, E and the not-yet-existing LS/LE should be touch-draggable on the waveform, not only encoder-driven. LVGL gives this via `LV_OBJ_FLAG_ADHESIVE`/drag events; the constraint work (ordering, clamping, minimum separation) is the real content.
2. ~~**`< Param` / `Param >`** replace `Param >` and `Refresh`.~~ Done.
3. ~~**Audition toggles to Stop**, matching the browser.~~ Done, and it stops on page exit — audition used to play on under a page that no longer existed.
4. **Encoder direction is a global contract, not a per-page choice.** Clockwise increases, always. The edit page shipped inverted because `InputEvent::delta` is already signed *and* the event type names the sign, so negating on the Left case flipped it back. Anything reading `delta` must take its magnitude and let the type supply direction. Worth a shared helper so the next page cannot repeat it.
5. **Two different physical controls are conflated.** `EncoderLeft`/`Right` come from the rotary encoder; `EncoderUp`/`Down` come from a pot (`ui_task.cpp`). Pages currently treat them as one input. Decide whether that is intended before building marker editing on top of it.

### 1.5.3 Sample browser

Port to the wireframe (design 1b): 54 px rows, green selection ring, per-row loading spinner for in-flight pagination, and the 474×521 detail panel with waveform, format/length/data-offset table and audition progress. The list rows live in the shared `wavex_file_browser` component, so this reaches beyond `ui_sample_browser.cpp`.

Keep the `data_start` offset row: `data_start % 4` correlated exactly with the stutter across three files. See `docs/backlog.md`, which warns specifically against "fixing" that by rounding `data_start` up — the mechanism is still unproven.

### 1.5.4 Busy feedback

Long operations look like a freeze. Before building an overlay, determine whether the UI is genuinely blocked: if a sample load runs on the UI task, LVGL never redraws and a spinner will not spin — a frozen spinner is worse than no feedback. If blocked, move the work off the UI task first.

Then a shared `ui_busy_overlay` (scrim + spinner + caption) used by every page, preferring a determinate bar where a total is known — `MSG_SAMPLE_LOAD` carries `sample_size` and `MSG_SAMPLE_STATUS` reports progress, so sample load can show a real percentage. Always pair with a timeout and an error path: a spinner that never resolves is indistinguishable from the freeze it was added to explain, and the backend can genuinely fail to answer (card pulled mid-load).

**Gate**: set all four markers on a multi-minute WAV, audition the looped region, save, reboot, reload, and hear the same region. Markers survive a power cycle; no UI freeze during audition, zoom or load.

---

## Phase 2 — Groovebox Core: Sequencer + Pads (see `features/sequencer.md`)

The host-testable core is built: `pattern.hpp`, `sequencer_scheduler.hpp`, `tempo_follower.hpp`, `sequencer_transport.hpp` (all HAL-free, ~60 host tests), and the 0x50–0x57 protocol messages with round-trip and dispatch tests.

Open:

1. **The audible half**: drive `SequencerTransport::Tick()` from the audio callback and turn `TriggerEvent`s into voice triggers. Needs the double-buffered edit-between-steps discipline (`sequencer.md` §4), a track→sample kit mapping (instrument model, Phase 2.5), and the intra-block sample-offset trigger path. Requires hardware audition to verify.
2. **MIDI clock out**: 0x57 serialization on the ESP32 to DIN + USB.
3. **UI**: pad grid page (TCA8418 matrix + touch), step editor page, kit editor. LED feedback via TLC5947 — bring up the SPI2 driver here, its first real consumer.
4. **Project persistence on SD** (kits/patterns/songs); atomic save (temp + rename). Use the WXCF chunk container (`features/instrument-model.md` §5; `firmware/shared/wxcf/wxcf.hpp` is built and host-tested), not a per-file format.
5. **P-lock application**: the pattern model already carries `param_locks[≤4]` and transport edits them; applying them to trigger params lands with the callback integration, per `features/param-locks-and-modulation.md` §2.

Kit representation is settled: a kit is a drum-mode instrument (`features/instrument-model.md` §8). Design Phase 2's kit structs so they *are* the drum-mode subset, not a parallel format to migrate later.

**Gate**: program and perform a 4-track drum pattern with swing from the front panel; MIDI-clock-synced to a DAW without audible drift over 10 minutes.

---

## Phase 2.5 — Sampler Instrument Layer (E-mu lineage)

Makes WaveX an *instrument* in the Emax/Emulator sense: multisampled presets across key/velocity ranges, a closed sampling loop, melodic sequencing, routed modulation. Index and rationale: `features/feature-expansion-ideas.md`.

Built already: the `VoiceManager` extensions, the instrument-model core (`audio/instrument.hpp` — zones, velocity layers, crossfade, choke, tuning fold; 18 host tests), and the WXCF container.

1. **Instrument model, remaining pieces** (`features/instrument-model.md`): the sample table that populates a `SampleResolver` from loaded WAVs; the `MSG_INST_OP/STATUS/ZONE_SYNC` protocol (0x60–0x62); deleting the Phase-1 stopgap note→sample policy; the ESP32 UI; the callback wiring shared with Phase 2 item 1; hardware audition.
2. **Mixer v1** (`features/output-routing-and-mixer.md` §1–2): 16-track gain/pan/mute/solo + per-track meters (0x78/0x79). Small, and the performance work below wants it.
3. **Melodic sequencing** (`features/melodic-sequencing.md`): melodic track type, chords/ties, step-record, live record/overdub/erase on the Daisy.
4. **Modulation matrix + LFOs + filter envelope** (`features/param-locks-and-modulation.md` §3–5): 8 slots/instrument, block-rate evaluation, `MSG_MIDI_CC` (0x56). Land the param slew engine (`features/scenes-and-performance.md` §3) here — same control-tick surface.
5. **Sampling/recording v1** (`features/sampling-and-recording.md`): threshold-armed capture with pre-roll, resample/bounce source, audition-before-save, non-destructive auto-trim markers, assign-to-zone. Depends on Phase 1 item 2.
6. **Arpeggiator** (`features/arpeggiator.md`): per-slot, clock-synced, latch; feeds live record.

**Gate**: build a multisampled keyboard instrument (≥ 3 key zones × 2 velocity layers) from freshly recorded samples entirely on-device; play it from MIDI through the Stage A analog path; live-record a chord progression + arp line over a drum pattern with p-locked filter moves; 1-hour zero-underrun soak with all of the above active; `make test` green.

---

## Phase 3 — Analog Voice Board (see `features/analog-voice-board.md`)

The **Stage A → Stage B transition** (`features/analog-voice-board.md` §0). The paraphonic prototype from Phase 1 already validated CV calibration, envelope→CV timing and analog levels, so this is hardware bring-up plus a flag flip, not new engine architecture. Blocked on the Stage B CV DAC part decision (`architecture.md` §3.3).

1. Decide Stage B CV DAC (recommendation: SPI MCP48CMB28 chain) and voice count; freeze PCB spec.
2. PCM1690 bring-up: SAI2 TDM-8 master TX, 8 test tones to verify slot order; I2C register init (reset sequencing, 24-bit TDM format, unmute).
3. `TdmVoiceSink` / `AudioOutputMode::VoiceSAI2` path: per-voice → TDM slot interleave; SAI1 stays stereo input + master mix.
4. `Mcp48Backend` behind the CV group router; flip flags to `TDM8` / `MCP48` / 8 groups; DMA/IT flush from the main loop — never blocking I2C/SPI in the callback.
5. Re-run calibration per voice (procedure and UI page reused from Stage A); stored calibration table on SD.
6. Keep Stage A buildable in CI as the fallback/bring-up configuration.

**Gate**: 8 analog voices with per-voice cutoff/res/VCA under sequencer control; calibration survives power cycle; scope-verified CV update ≤ 1 ms after control tick.

---

## Phase 4 — Offline Sample Editing & Mangling (see `features/offline-sample-editing.md`)

Recording ships in Phase 2.5; non-destructive marker editing ships in Phase 1.5. This phase adds the destructive half. Render-job messages use the reserved 0xA0–0xA3 block.

1. Render-job scheduler on the Daisy main loop (chunked SD→SD processing with progress messages; cancellation).
2. Editing primitives: trim/crop, gain/normalize, fades, reverse, mono↔stereo, resample, **crossfade-loop render** (`xfade_loop` — seam-smoothing for zone loops, the Emax tool; `features/instrument-model.md` §11).
3. Waveform editor UI, destructive half: decimated preview tiers cached in ESP32 PSRAM, destructive ops via render jobs. The non-destructive marker UI is Phase 1.5; this extends it rather than replacing it.
4. Slicing: transient detection (offline), slice-to-pads workflow.
5. Mangling effects (offline renders): bit-crush, drive/saturate, time-stretch, pitch-shift, granular freeze. CMSIS-DSP kernels where applicable — where the 1.17.0 upgrade pays off.

**Gate**: record → trim → normalize → slice → assign to pads → sequence, entirely on-device, with audio playback uninterrupted during renders.

---

## Phase 5 — Performance & Polish

- Song mode / pattern chaining; performance macros (encoder-assignable) + **scenes with morph** (`features/scenes-and-performance.md`).
- Digital send FX (delay, reverb) in the stereo master section.
- **Tuning & scales** (`features/tuning-and-scales.md`): master tune, 12-degree tables, scale-constrained input surfaces. Small and independent; slot it wherever a gap appears after Phase 2.5 item 1.
- Preset/kit browser richness (tagging, favorites), USB sample import (MSC or MTP — decide), settings persistence.
- ESP-IDF 6.0 migration (see 0.1).
- CPU/memory headroom pass with DWT profiling; lock the final block-size and clock decisions.
- **Polyphase sample-rate conversion** to replace `LinearResampleFrames`. Every non-48 kHz file is resampled by scalar `arm_linear_interp_q15` calls, one per output sample per channel — correct but aliasing-prone (linear interpolation is a poor anti-imaging filter) and ~1.7 ms per ~1050-frame chunk. The common case, 44.1 → 48 kHz, is the rational ratio 160/147, so a polyphase FIR with 160 precomputed phases on the CMSIS-DSP `arm_fir_*_q15` kernels replaces per-sample interpolation with a filter bank. Two wins (quality and CPU), and it removes the ≥2-input-frame edge cases behind the 2026-08 audition deadlocks. **Not urgent**: measured at roughly an 8% duty cycle during audition, this is not the bottleneck — the ~2.9 ms 8 KB `f_read` is the larger half. Verify against a measured profile before starting, and keep the linear path for ratios that are not usefully rational.

---

## Outstanding hardware verification

Code-complete but unproven. Each is real work, not history — a build that links is not a feature that works.

| What | Why it matters | How to check |
|---|---|---|
| LVGL 9.5 + `CONFIG_LVGL_PORT_ENABLE_PPA` with rotation | The original note claimed we don't use rotation. **That was wrong** — `display_manager.cpp` sets `LV_DISPLAY_ROTATION_90`, and PPA has known interaction bugs in rotation modes on P4. We are not exempt. | Flash the HX8394/MIPI-DSI panel, watch for tearing/corruption during rotation. Revert `CONFIG_LVGL_PORT_ENABLE_PPA` in both sdkconfig files if it misbehaves. |
| LVGL 9.5 performance claim | The upgrade was justified partly on speed; nothing has measured it. | FPS + UI-task CPU before/after on the waveform-preview and meter pages, per `docs/performance_monitoring.md`. |
| Partition table move | App offset moved 0x10000 → 0x20000. NVS content survives only because `nvs` kept its offset/size. | Flash and boot; confirm settings persist. |
| 48 kHz engine + resample path | 44.1 kHz content now always goes through the resampler — it is the normal path, not the exception. | Audition a 44.1 kHz and a 48 kHz WAV; confirm correct pitch on both. |
| UART full-duplex DMA + IRQ priorities | Async TX and the priority inversion fix are compile-verified only. | Sustained traffic during SD streaming; DWT jitter measurement. |
| SD soak on libDaisy v8.1.0 | The SDMMC/fatfs glue changed. | Mount, 1000× sequential reads, hot-unmount. |
| Stage A paraphonic analog path | Item is code-complete; all bench work outstanding. | CV-update-within-tick scope check, SSI2164 inversion, "silent is truly silent", exponential cutoff feel k≈3, analog levels. The CV Calibration page is the tool for this. |
| MIDI in-to-sound latency | Budget is ~2–3 ms on paper. | Measure DIN and USB in-to-sound on the bench; target < 5 ms. |
| Diagnostics telemetry round trip | `MSG_DIAG_PUSH` is implemented on both ends but never observed on hardware. | Open the diagnostics page; a non-zero `DIAG_PUSH` row in the Link message table proves it. |
| `ui-update` branch UI fixes | Tab tracking, edit-page threading, WAV duration, CPU tiles — all diagnosed by reading code. | See `docs/ui-update-backlog.md`. |

---

## Cross-Cutting Rules (apply to every phase)

- Every protocol change: update `protocol.h` + round-trip test + `features/inter-mcu-protocol.md` in the same commit.
- Every DMA buffer: alignment + placement per `architecture.md` §7 — reviewer checklist item.
- Every phase gate includes: 1-hour zero-underrun soak, both-MCU-reboot recovery test, `make test` green.
- Docs: new subsystems get a `docs/features/*.md` design doc **before** implementation; superseded docs move to `docs/archive/` (never silently deleted).
- **Nothing in the UI task may block**, and nothing outside it may touch LVGL. Backend callbacks arrive on the UART RX task: store and flag, then draw from an `lv_timer`. Both rules have been broken and both froze the display.
- **Completed work leaves this document.** Detail goes to `CHANGELOG.md`; anything still unproven goes to § Outstanding hardware verification. A roadmap that accumulates finished items stops being read.
