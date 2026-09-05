# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-05.

This document lists only open work. Completed work belongs in `CHANGELOG.md`
and git history. Code-complete but unverified hardware behavior remains open in
[Outstanding hardware verification](#outstanding-hardware-verification).

Phases are ordered by dependency, not date. Read `architecture.md` before
changing system behavior.

## Phase 0 — Foundation hardening

1. Replace the fixed-delay sample-retirement path with a callback
   acknowledgement; prove unload/reload cannot free memory referenced by a
   voice.
2. Publish callback telemetry as complete immutable snapshots before the main
   loop reads it.
3. Reduce overlapping ownership in the ESP32 application and extract focused
   modules only as relevant code changes.
4. Pin ESP-IDF to a 5.5 tag, then run the SD soak and panel checks. Treat an
   ESP-IDF 6 migration as a separate spike.
5. Keep CMSIS-DSP aligned with libDaisy. Neither it nor DaisySP is compiled
   into the image today (nothing used them); the CMakeLists notes how to add
   either back when a Phase 4/5 kernel (FFT, polyphase FIR, a band-limited
   oscillator) needs it. Revisit only when upstream moves or such a kernel
   requires a newer version. Update libDaisy only for a
   Phase 3 need or a released upstream tag.
6. The live transport is UART. SPI revival remains blocked by the six defects
   in [backlog.md](backlog.md#spi-link-revival-is-gated-on-six-recorded-defects).

**Gate:** clean `make all` and `make test`; SD soak passes.

## Phase 1 — Solid playback core

1. Add admission-controlled concurrent streamed voices; the current SD/ring
   path is singleton-only.
2. Rebuild recording against the voice/streaming architecture with fixed
   allocations outside the audio callback.
3. Revisit SPI bandwidth only after SPI revival is approved and bench-proven.

**Gate:** eight MIDI-played voices run for one hour with zero underruns; the
digital filter and gain are panel-controllable; both output/CV configurations
compile; host tests cover allocation and sample-load flow.

## Phase 1.5 — Sample editing

The non-destructive marker, fade, metadata, and waveform foundations are
complete. Remaining work:

1. Persist marker/gain edits with the WXCF sidecar model; settle Save As naming
   and whether it creates a sidecar or waits for a render-to-new-file path.
2. Give Sample Edit explicit sample selection and define partial-load behavior
   for samples too large for resident RAM.
3. Finish marker interaction: loop seam verification, zero-crossing snap
   protocol/policy for stereo, and playback-time loop crossfade.
4. Reconcile stereo behavior between streaming and RAM voices; add the UI for
   `channel_mode`, including a label for one-channel views.
5. Retire the legacy decimated preview when the Record page is rebuilt.

**Gate:** edit and audition a multi-minute WAV, save, reboot, reload, and hear
the same region without a UI freeze.

## Phase 2 — Groovebox core: sequencer and pads

The scheduler and protocol core are host-tested. Open work:

1. Drive `SequencerTransport::Tick()` from the audio callback and turn events
   into sample-offset voice triggers with double-buffered edit-between-steps
   handling.
2. Serialize MIDI clock out on the ESP32's DIN and USB paths (needs 2.P.5).
3. Build the pad grid, step editor, kit editor, and TLC5947 LED feedback
   (needs 2.P.1–3).
4. Persist kits, patterns, and songs atomically through WXCF.
5. Apply per-step parameter locks to trigger parameters.

### 2.P — Panel controls and MIDI I/O (prerequisite for items 2 and 3)

Design: `features/panel-controls.md` (decided 2026-09-05). Today the panel
is touch plus two PCNT encoders and four mapped keys; no LED or pot driver
exists, and DIN MIDI is compiled out because its RX pin was the flash port.
Stages, one commit each:

1. `PanelKey`/`PanelLed` model and key map: logical keys for the six
   softkeys, Shift, the root-menu jump keys, Track ±, transport and pads;
   `SoftkeyBar::press(n)`, `UINavigator::jumpToRoot()`, `KEY <name>` on
   the console, a Diagnostics Panel tab.
2. TCA8418 interrupt-driven keypad task (fallback poll retained).
3. `panel_task` owning SPI2: TLC5947 chain, LED policy, `LEDS` in `STATE`;
   absorbs `pcnt_task`.
4. MCP3008 + endless-pot decoder (host-tested), calibration store, the
   four-`EncoderBinding` page contract and strip widget; first consumers
   are the Instrument and Play pages.
5. MIDI: DIN back on at its new pins, UART2 TX ring shared with USB MIDI
   out, DIN/USB latency measured.

Stage 0 (pin reconciliation against the ESP32-P4-WIFI6 header, MIDI pins
moved, CD74HC4067 dropped) and stage 1 (`PanelKey`/`PanelLed`, the key
map, jumps, softkeys and Track ± from the panel, the Diagnostics Panel
tab) landed 2026-09-05.

**Gate (2.P):** from the panel alone — jump to Instrument, change cutoff on a
pot and hear it, latch Shift and fire a shifted softkey, BACK out — with
every LED correct throughout; DIN and USB MIDI notes sound; `make test` and
`make test-hil` green.

**Gate:** program and perform a four-track pattern with swing from the panel;
remain MIDI-clock-synced to a DAW for ten minutes without audible drift.

## Phase 2.5 — Sampler instrument layer

The model core, SFZ import, WXCF container, the shared selected Track, and
parts of the mixer/modulation path are built. The end state is
`features/track-and-patch-model.md` (Track / Instrument / Bank / Sample Pool;
the two-oscillator Instrument is designed there, §3). Stages 1 (rename),
2 (Load-to-Track), 3 (Sample Pool) and 7 (Track model and MIDI routing) are
done. Open work, in the order decided 2026-09-05 (model doc §8: 7 → 4 → 5 →
6 → 8), with 7 now closed:

1. Instrument file and editors (stage 4): `.wxi` with the full chunk set,
   Init/Save/Name ops, Pad Map (first) and Key Map (with Sample Manager
   "to pad"), Instrument Browser, Track page.
2. Voice architecture (stage 5): typed oscillators, Osc 2 + submix, filter
   type, Env 3, two per-voice LFOs, new mod destinations —
   DWT-measured at `WAVEX_NUM_VOICES` before the count is changed.
3. Bank (stage 6): `.wxb`, Bank page, Program Change recall.
4. Polyphony policy (stage 8), from stage 5's measurement.
5. Finish Mixer v1: UI/solo behavior, meter subscription, and hardware click
   and soak tests.
6. Add melodic sequencing, chord/tie handling, step/live record, and erase.
7. Complete modulation: p-lock application, MIDI CC/channel-pressure
   forwarding, and modulation UI (the per-voice LFOs and zone filter ADSR
   move into stage 5).
8. Build sampling/recording v1 and the arpeggiator.

**Gate:** from power-on, hear a card sample on the Keys in four taps; build a
16-pad kit and a multisampled keyboard Instrument on-device and save both;
load a Bank and recall a slot from a MIDI Program Change; record a chord
progression and arp over a p-locked drum pattern; complete a one-hour
zero-underrun soak with both oscillators active at `WAVEX_NUM_VOICES`.

## Phase 3 — Analog voice board (deferred)

No analog hardware is currently planned. Keep the Stage B design and both build
configurations viable. If revived: choose the CV DAC and voice count, prove
PCM1690 TDM slot order, wire `TdmVoiceSink` and `Mcp48Backend`, calibrate every
voice, and verify CV update timing on a scope.

**Gate:** eight analog voices under sequencer control; calibration survives a
power cycle; CV updates arrive within one control tick.

## Phase 4 — Offline editing and mangling

Build a bounded Daisy-main-loop render-job scheduler, destructive editing and
crossfade-loop rendering, editor integration, slicing, and offline mangling.
Use the reserved 0xA0–0xA3 messages and CMSIS-DSP where appropriate.

**Gate:** record, edit, slice, assign, and sequence entirely on-device while
playback remains uninterrupted.

## Phase 5 — Performance and polish

Add song mode, scenes/macros, send effects, tuning/scales, Instrument/Bank
browser quality (Bank Select, "save with samples"), settings persistence, USB
sample import, and a measured CPU/memory headroom pass. Consider polyphase sample-rate conversion only after profiling
shows linear interpolation is a meaningful cost or quality limit.

## Outstanding hardware verification

The following code paths are open until observed on the target:

| Area | Verification |
|---|---|
| Display rotation/PPA | Check the HX8394 panel for corruption or tearing. |
| Partition migration | Flash, boot, and confirm settings persist. |
| Audio formats | Audition 44.1 and 48 kHz WAVs; confirm pitch. |
| UART and SD | Sustain traffic during streaming; run read and hot-unmount soak tests. |
| MIDI latency | Measure DIN and USB input-to-sound latency; target under 5 ms. USB MIDI enumerates on the USB 2.0 HS controller — the board's 4-pin USB connector, not the Type-C — and that has never been confirmed on the bench. DIN waits on 2.P.5 (receiver on the new RX pin). |
| Panel pins (2026-09-05) | `pin_config.h` was rewritten against the ESP32-P4-WIFI6 header. The bench encoder is PCNT unit 1 (confirmed 2026-09-05); it counts negative on clockwise as wired, and three pages had compensated for it — direction is now one per-encoder flag in `hardware_config.h`, and those pages follow the shared contract. Clockwise increases values / moves forward on every page — verified 2026-09-05. Verify the TCA8418 matrix geometry (`WAVEX_TCA8418_ROWS/COLUMNS`, never confirmed against the wiring) and the `WAVEX_KEYCODE_*` map from the Diagnostics ▸ Panel tab (2.P.1): press each key, read its keycode, row/column and `PanelKey`; "unmapped" means the map or the geometry is wrong. Blocker first: the bench log shows `TCA8418 hardware initialization failed` on every boot recorded (2026-09-05), so the keypad has not been answering on I2C at all — check its wiring and address before reading anything off the Panel tab. Scope an endless pot's two wipers before calibrating (the decoder assumes triangle waves). |
| Diagnostics | Open the page and verify live telemetry arrives. |
| Digital voices | Trigger RAM-resident notes, sweep live parameters, and judge SVF response/resonance. |
| Callback budget | Establish the first recurring callback-headroom report: DWT-measure SVF (both topologies, 24 dB, drive), DTCM placement, mixer, and 480 MHz behavior with eight voices on the persistent QSPI `-O2` image, plus a zero-underrun soak. Record it in `callback-performance-log.md` using the gate in `performance_monitoring.md`. |
| Sample Edit | Verify waveform fetch, handles, loop seam, browser detail waveform, and stereo readability. **Loop playback does not work** (bench, 2026-09-05): a sample with loop points set plays through in both Sample Edit audition and Play; not yet traced (is the loop sent, stored on the Track, or honoured by the voice?). See `backlog.md` § Sample loop playback. |
| Settings and input | Verify brightness, scrolling, MIDI channel filtering, keypad, encoder direction, and UI responsiveness. |
| UI concurrency | Measure LVGL lock/refresh behavior during encoder bursts and sample loading. |
| Load-to-Track and the Sample Pool (2026-09-04/05) | Automated: `make test-hil` (`tests/hil/test_load_to_track.py`, `test_sample_pool.py`) covers Load onto an empty Track and a note sounding on it, the replace picker (cancel, confirm, Track -/+), Assign's confirm, a failed load's reason crossing the link, one file loaded twice being one Pool entry, an import's samples in the Pool and playing, two imports of one file set sharing it, a sample replacing an import and freeing what only it held, and the Sample Manager paging a Pool larger than one page. Still manual: *hearing* the Keys, and a Pool-full / arena-full refusal (no card holds 1024 samples or 60 MB of small ones). |
| Image slimming (2026-09-04) | Boot, mount SD, stream a WAV, trigger RAM voices and run the sequencer on the 273 KB image: large callback/loader state is now constructed at startup (`bss_static.hpp`) instead of copied from `.data`, the SD volume links `SD_Driver` directly, and `UART_LOGx` lines should now appear in the log. DWT-measure `Render()` with region fades set (the per-sample 64-bit divide is gone; expect a drop, no number yet). |

## Rules for every phase

- Apply the decision filter in `project-principles.md`.
- For each protocol change, update `protocol.h`, add round-trip tests, and
  update `features/inter-mcu-protocol.md` in the same commit.
- Follow `architecture.md` DMA/cache rules and keep audio callbacks nonblocking.
- Run and report the callback-headroom gate in `performance_monitoring.md` at
  every phase gate and at its event/calendar triggers. `< 70%` worst-case stays
  on the H750; `70-<80%` blocks for investigation; `>= 80%` with planned
  callback features remaining activates the backend chip-upgrade path.
- Every phase gate includes `make test`, `make test-hil` on the bench, reboot
  recovery, and an appropriate zero-underrun soak.
- New subsystems need a focused feature design before implementation.
- Delete superseded docs after moving any remaining open work here or to the
  backlog; git history is the archive.
