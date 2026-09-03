# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-02.

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
5. Keep CMSIS-DSP aligned with libDaisy. Revisit only when upstream moves or a
   Phase 4/5 kernel requires a newer version. Update libDaisy only for a
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
2. Serialize MIDI clock out on the ESP32's DIN and USB paths.
3. Build the pad grid, step editor, kit editor, and TLC5947 LED feedback.
4. Persist kits, patterns, and songs atomically through WXCF.
5. Apply per-step parameter locks to trigger parameters.

**Gate:** program and perform a four-track pattern with swing from the panel;
remain MIDI-clock-synced to a DAW for ten minutes without audible drift.

## Phase 2.5 — Sampler instrument layer

The model core, SFZ import, WXCF container, and parts of the mixer/modulation
path are built. Open work:

1. Complete instrument editing and management: pad-to-sample mapping, named
   patch save/load, editable zones, and the selected Track/Patch UI model.
2. Finish Mixer v1: UI/solo behavior, meter subscription, and hardware click
   and soak tests.
3. Add melodic sequencing, chord/tie handling, step/live record, and erase.
4. Complete modulation: p-lock application, per-voice LFO, zone filter ADSR,
   MIDI CC/channel-pressure forwarding, and modulation UI.
5. Build sampling/recording v1 and the arpeggiator.

**Gate:** build and play a multisampled keyboard instrument on-device, record
a chord progression and arp over a p-locked drum pattern, and complete a
one-hour zero-underrun soak.

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

Add song mode, scenes/macros, send effects, tuning/scales, preset-browser
quality, settings persistence, USB sample import, and a measured CPU/memory
headroom pass. Consider polyphase sample-rate conversion only after profiling
shows linear interpolation is a meaningful cost or quality limit.

## Outstanding hardware verification

The following code paths are open until observed on the target:

| Area | Verification |
|---|---|
| Display rotation/PPA | Check the HX8394 panel for corruption or tearing. |
| Partition migration | Flash, boot, and confirm settings persist. |
| Audio formats | Audition 44.1 and 48 kHz WAVs; confirm pitch. |
| UART and SD | Sustain traffic during streaming; run read and hot-unmount soak tests. |
| MIDI latency | Measure DIN and USB input-to-sound latency; target under 5 ms. |
| Diagnostics | Open the page and verify live telemetry arrives. |
| Digital voices | Trigger RAM-resident notes, sweep live parameters, and judge SVF response/resonance. |
| Callback budget | DWT-measure SVF, DTCM placement, mixer, and 480 MHz behavior with eight voices. |
| Sample Edit | Verify waveform fetch, handles, loop seam, browser detail waveform, and stereo readability. |
| Settings and input | Verify brightness, scrolling, MIDI channel filtering, keypad, encoder direction, and UI responsiveness. |
| UI concurrency | Measure LVGL lock/refresh behavior during encoder bursts and sample loading. |

## Rules for every phase

- Apply the decision filter in `project-principles.md`.
- For each protocol change, update `protocol.h`, add round-trip tests, and
  update `features/inter-mcu-protocol.md` in the same commit.
- Follow `architecture.md` DMA/cache rules and keep audio callbacks nonblocking.
- Every phase gate includes `make test`, reboot recovery, and an appropriate
  zero-underrun soak.
- New subsystems need a focused feature design before implementation.
- Delete superseded docs after moving any remaining open work here or to the
  backlog; git history is the archive.
