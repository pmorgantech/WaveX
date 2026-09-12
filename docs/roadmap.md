# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-11.

This document lists only open work. Completed work belongs in `CHANGELOG.md`
and git history. Code-complete but unverified hardware behavior remains open in
[Outstanding hardware verification](#outstanding-hardware-verification).

Phases are ordered by dependency, not date. Read `architecture.md` before
changing system behavior.

## Phase 0 — Foundation hardening

1. Reduce overlapping ownership in the ESP32 application and extract focused
   modules only as relevant code changes.
2. Pin ESP-IDF to a 5.5 tag, then run the SD soak and panel checks. Treat an
   ESP-IDF 6 migration as a separate spike.
3. Keep CMSIS-DSP aligned with libDaisy. The DaisySP SVF implementation is
   compiled for the filter comparison; add other kernels only when used. Revisit only when upstream moves or such a kernel
   requires a newer version. Update libDaisy only for a
   Phase 3 need or a released upstream tag.
4. The live transport is UART. Dormant SPI source fixes do not satisfy the
   [hardware revival gate](backlog.md#spi-link-revival-requires-hardware-verification).

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

**Gate:** edit and audition a multi-minute WAV, save, reboot, reload, and hear
the same region without a UI freeze.

## Phase 2 — Groovebox core: sequencer and pads

The scheduler, protocol and callback trigger path exist. The 16 pattern rows
address their matching Tracks at each step's selected MIDI note, with
velocity-aware prepared zone selection. Melodic chords and gate lanes remain
Phase 2.5 work. Open work:

1. Verify sample-offset timing and edit boundaries on hardware for the
   four-track gate, including the prepared note/velocity resolution.
2. Serialize MIDI clock out on the ESP32's DIN and USB paths (needs 2.P.5).
3. Complete TLC5947 LED feedback. Per-pad cutoff and amp attack/decay/sustain
   editing with inheritance reset is implemented. The
   touch kit editor provides creation, naming, assignment, choke and new-copy
   WXI saves. The touch Play pads
   and sequencer grid are built; the grid pages across all 16 Tracks and 64
   steps and edits tempo, swing, length, scale, velocity, probability, note and mute.
   Physical controls/LED feedback require 2.P.1–3; touch workflows do not.
4. Complete song/project persistence through WXCF. Kit WXI saves and named
   pattern WXCF save/load are implemented; pattern files preserve tempo and
   Track instruments, include hidden steps, and use checked temp/rename
   new-copy saves. The
   [Project codec](features/project-persistence.md) now preserves sparse
   Patterns, Songs and Performance metadata in bounded foreground records;
   SD transactions, Instrument snapshots and session restore remain to be
   connected. Power-loss recovery remains a bench gate.
5. Voice-scoped per-step locks and the touch Locks editor are implemented:
   cutoff, resonance, amp ADSR, pitch, pan, gain and sample/loop start. Host
   tests and a two-board edit/save/recall test pass. Live lock recording and
   analog/group lock lifetimes remain separate work; callback capacity and the
   complete phase gate still require their measured acceptance.

### 2.C — Callback capacity checkpoint

The clean expanded-envelope workload on db6f180 peaked at **66.9571% (STAY)**
over 605.2 seconds (121 windows), with zero underruns and six successful
pattern save/load cycles. It retains the two-source workload below and adds
saved Env 2/3 settings and 64 routes using live sources, including Env 1/3.
The image SHA and complete readback are recorded in
[the performance log](callback-performance-log.md). This permits the per-voice
LFO milestone; it does not close the one-hour phase soak.

Previous two-source checkpoint:

The clean two-source workload on 409e40c peaked at **65.6921% (STAY)**
over 605.2 seconds (121 windows), with zero underruns. All eight Tracks use
two 16-zone maps, with four locks per hit, 64 modulation slots, WaveX 24 dB
filter/full drive, live cutoff edits, SD streaming and pattern save/load cycles.
The renderer and event paths run from ITCM; source-reading calls are inlined.
Capture and image attribution are recorded in
[the performance log](callback-performance-log.md). This permits the next
expanded-voice milestone; every further callback feature still needs its
own measured checkpoint and the full phase soak remains open.

Previous applied-lock checkpoint:

The applied-lock workload on 3dee3bf peaked at **69.6971% (STAY)** over
605.2 seconds with zero underruns and six successful pattern save/load cycles.
It uses the identical audio image committed in 5d9683a: four voice locks per
hit and filter/envelope initialization moved from note-on to startup. This
supersedes the fresh pre-lock 70.0479% REVIEW baseline for continuation.
Remeasure each expanded voice milestone before proceeding beyond it. During pattern-load transitions,
the transport is stopped and one-shot voices may finish naturally; eight
voices are checked after each restart.

Previous checkpoint (2026-09-10):

The per-pad sound persistence workload on f904032 measured **69.4104% peak
callback utilization (STAY)** over 605.2 seconds, with six successful pattern
save/load cycles, zero underruns, no file errors and no sequencer queue-drop
messages. It used eight drum Instruments with sixteen populated pads each,
Track-local choke, 64 modulation slots, the WaveX 24 dB filter at full drive,
25 MHz SD streaming, live Instrument and pad cutoff edits and grid readback.
The streaming preview stopped for each file operation and restarted afterward;
resident Track voices continued. Combined cutoff/resonance updates reduced
the observed peak from the pad-editor baseline's 70.7815% (REVIEW).
Full captures and preceding comparisons are recorded in
[callback-performance-log.md](callback-performance-log.md).

The earlier SD-write failure no longer blocks this workload after the default
clock reduction and authorized preservation/recreation of the test directory.
A newly saved pattern and a kit with pad sound overrides loaded after backend
restart on normal firmware.
Arbitrary power-loss recovery, longer write soaks and the full Phase 2 gate
remain outstanding.

The DaisySP comparison measured 89.6635% for 605.2 seconds with zero
underruns and entered UPGRADE because callback-resident work remains.
This activates planning in the [RT1170 migration plan](rt1170-migration.md)
under the Phase 2 capacity checkpoint. It does not authorize a board port or
purchase, and the WaveX path remains the fallback.

DaisySP is default-disabled; comparison builds require an explicit opt-in
and do not pass the capacity gate. Re-enabling it requires attribution,
scope reduction or a backend port. Re-run the checkpoint before adding
callback scope. Parameter locks remain item 5 and were programmed but not
applied by either measured callback.

### 2.P — Panel controls and MIDI I/O (physical integration)

Design: `features/panel-controls.md` (decided 2026-09-05). The physical
panel is not wired yet (confirmed 2026-09-07). Touchscreen work can continue,
including the Phase 2 kit editor using the Phase 2.5 Instrument model.
The user authorized stage 4 touchscreen editors to proceed on 2026-09-11:
general key/velocity zones, Instrument Browser and Track page. This does not
close the Phase 2 panel or timing gates.
PCNT encoder support and the logical key map exist in firmware; their
physical integration, LED/pot drivers and DIN MIDI remain deferred.
Stages, one commit each:

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

1. Requested core Instrument editors (stage 4) built 2026-09-11: Key Map
   exposes 32 stable slots with resident assignment/clear, staged key/velocity
   ranges, root notes and WXI save copies. Instrument Browser filters WXI/SFZ
   with preflight and confirmed Track loading. Track page selects eight Tracks
   per view and edits MIDI input with authoritative readback. Pad Map supports
   creation, naming, assignment, choke and per-pad cutoff/amp ADS overrides.
   Tag metadata/filtering remains future work; these editors do not close the
   Phase 2 hardware gate.
2. Voice architecture (stage 5): Osc 2 sample submix, its touch settings,
   both keyboard zone maps, Env 1–3 and the eight-row matrix editor are built.
   Complete filter type, two per-voice LFOs and new mod destinations —
   DWT-measured at `WAVEX_NUM_VOICES` before the count is changed.
   The planned LFO tab adds waveform, frequency, pitch-follow and gate/free-run
   controls, taking the stage bar from five to six. Build it with the runtime
   and typed transport, and measure each callback milestone before continuing.
3. Bank (stage 6): the [WXCF Bank codec](features/bank-persistence.md) and
   bounded resident index are host-tested. SD working-copy management, Bank
   page, Track recall, preload and Program Change recall remain open and follow
   the expanded Instrument engine.
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
| Sample retirement | Four-Track routing, rebinds and SFZ replacement during sequencing, and sample-edit refresh now have passing HIL coverage. Still exercise delayed/stopped callbacks and concurrent import requests. Confirm timeout preserves storage, then measure DWT headroom and run the zero-underrun soak. Host helper tests do not verify this interrupt integration. |
| Callback budget | Recurring whole-callback evidence is recorded in `callback-performance-log.md`: The eight-Track, fully populated kit/grid WaveX run with pad sound edits and pattern save/load is 69.4104% / STAY and the experimental DaisySP comparison is 89.6635% / UPGRADE, both with zero underruns. DaisySP is default-disabled. Remaining work is its capacity blocker, separate Render() timing, and selective placement A/B; no overall phase gate is claimed. |
| Sample Edit | Verify waveform fetch, handles, loop seam audibility, browser detail waveform, and stereo readability. HIL covers Track-preserving audition, edits isolated to the matching stream, streaming loop wraps and RAM-loop note lifetime. |
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
