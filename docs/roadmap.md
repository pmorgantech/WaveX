# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-17.

This document is the single planning list: scheduled phases, next steps,
unscheduled backlog and open decisions. Completed work belongs in `CHANGELOG.md`
and git history. Code-complete but unverified hardware behavior remains open in
[Outstanding hardware verification](#outstanding-hardware-verification).

Runnable bench procedures and per-case results live in
[hardware-validation.md](hardware-validation.md). Add or update an entry there
in the same change whenever implemented work reaches a physical gate. This
roadmap retains implementation priority and phase acceptance; the checklist
does not replace or relax those gates.

Phases are ordered by dependency, not date. Read `architecture.md` before
changing system behavior. Promote backlog items into a phase when its gate
requires them; keep one task here rather than parallel lists in separate files.

## Contents

- [Phase 0: foundation](#phase-0--foundation-hardening)
- [Phase 1: playback](#phase-1--solid-playback-core)
- [Phase 1.5: sample editing](#phase-15--sample-editing)
- [Phase 2: sequencer and pads](#phase-2--groovebox-core-sequencer-and-pads)
- [Phase 2.5: Instruments, Banks and Mixer](#phase-25--sampler-instrument-layer)
- [Phase 3: analog board](#phase-3--analog-voice-board-deferred)
- [Phase 4: offline editing](#phase-4--offline-editing-and-mangling)
- [Phase 5: performance and polish](#phase-5--performance-and-polish)
- [Hardware verification](#outstanding-hardware-verification)
- [Rules for every phase](#rules-for-every-phase)
- [Next steps and backlog](#next-steps-and-backlog)

## Phase 0 — Foundation hardening

1. Reduce overlapping ownership in the ESP32 application and extract focused
   modules only as relevant code changes.
2. Pin ESP-IDF to a 5.5 tag, then run the SD soak and panel checks. Treat an
   ESP-IDF 6 migration as a separate spike.
3. Keep CMSIS-DSP aligned with libDaisy. No DaisySP module is linked into
   the firmware: its ladder is ported first-party (`ladder_huovilainen.hpp`,
   MIT) and the vendored copy is built only as the host-test reference; add
   other kernels only when used. Revisit only when upstream moves or such a kernel
   requires a newer version. Update libDaisy only for a
   Phase 3 need or a released upstream tag.
4. UART remains the production transport. The opt-in macro experiment and
   [cutover evidence](spi-notes.md#verification-and-remaining-gates) do not close
   the [hardware revival gate](roadmap.md#spi-link-revival-requires-hardware-verification):
   production adoption still needs timing traces, fault injection and a long soak.
   The authorized [hybrid design](features/hybrid-inter-mcu-link.md) keeps
   controls and confirmations on UART while evaluating SPI data/telemetry; its
   implementation and session-recovery gates remain open.

**Gate:** clean `make all` and `make test`; SD soak passes.

## Phase 1 — Solid playback core

1. Add admission-controlled concurrent streamed voices; the current SD/ring
   path is singleton-only.
2. Rebuild recording against the voice/streaming architecture with fixed
   allocations outside the audio callback.
3. Revisit the production SPI bandwidth decision only after its full hardware
   revival gate passes.

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
2. MIDI clock in/out and sync are implemented (2026-09-17): Daisy 24-PPQN
   output, ESP32 timestamped DIN/USB ingest, first-source selection, next-Clock
   Start/Continue, SPP Pattern/Song relocation and sequencer clock controls.
   Next: measure jitter, callback cost and DAW sync in HV-014/HV-005. The full
   gate remains open. LFO Rate now switches between 0.01–100 Hz and
   musical durations including 3/16, preserving Hz across Sync toggles; see
   [the design](features/param-locks-and-modulation.md#lfo-rate-controls).
   Audible behavior, rendering and callback cost remain open in HV-015.
3. Validate TLC5947 LED feedback on hardware (HV-012). Per-pad cutoff and amp attack/decay/sustain
   editing with inheritance reset is implemented. The
   touch kit editor provides creation, naming, assignment, choke and new-copy
   WXI saves. The touch Play pads
   and sequencer grid are built; the grid pages across all 16 Tracks and 64
   steps and edits tempo, swing, length, scale, velocity, probability, note and mute.
   Physical controls/LED feedback require 2.P.1–3; touch workflows do not.
4. Song/Project persistence and touch composition are software-complete:
   checked new-copy Project transactions restore Instruments, sample edits,
   Pattern slots, Songs, mixer and session settings. Named Pattern files and Kit
   WXI saves remain available. [Pattern management](features/pattern-management.md)
   creates/names/copies slots and launches at the next full-loop boundary.
   [Song sequencing](features/song-sequencing.md) arranges stable references with
   repeats, insert/remove/reorder, selected-section starts, looping and playing
   section feedback on the Daisy clock. Physical timing, callback capacity,
   save/reboot recovery and panel validation remain open in
   [HV-007–009](hardware-validation.md). This does not close the Phase 2 gate.
5. Voice-scoped per-step locks and the touch Locks editor are implemented:
   cutoff, resonance, amp ADSR, pitch, pan, gain and sample/loop start. Host
   tests and a two-board edit/save/recall test pass. Live lock recording and
   analog/group lock lifetimes remain separate work; callback capacity and the
   complete phase gate still require their measured acceptance.

### 2.C — Callback capacity checkpoint

Current stereo checkpoint (2026-09-16): three ten-minute captures on the
unchanged `8714ad0` Daisy image filled the default eight-channel budget.
Eight downmixed voices peaked at **66.5060% (STAY)**; four stereo voices at
**50.3408%**, and two stereo plus four Mono voices at **57.6129%** (both
COMFORTABLE). All completed ten Pattern save/load cycles with zero reported
underruns and dropped console bytes. The new harness uses two oscillators,
modulation, locks, streaming and live filter edits; its one-zone maps differ
from the older sixteen-zone workload, so this is capacity evidence, not a
matched before/after comparison. See the
[stereo evidence](callback-performance-log.md#stereo-channel-verification--2026-09-16).
The one-hour soak and complete phase gate remain open.

Pre-stereo checkpoint (2026-09-13): the accepted callback/modulation ITCM
placement was re-baselined with three unchanged-image ten-minute captures.
The four DSP candidates were then retested in order; only voice-owned
modulation exponent caching was adopted after two full confirming captures.
Its worst observed callback was **65.0448% (STAY)**, with a repeatable
approximately 0.2% average improvement over the ITCM baseline and no claimed
peak improvement. The full eight-voice workload combines two source maps,
Env 1-3, sixteen voice LFOs, 64 routes, four locks per hit, live editing,
streaming, the touch grid and file operations. Every valid capture completed
with zero reported underruns and console RX dropped bytes.
[The performance log](callback-performance-log.md) records exact images,
compiler/profiler controls, repeats and rejected candidates. The one-hour soak
and complete Phase 2 gate remain open.

Previous expanded-envelope and LFO checkpoints:

The clean expanded-envelope workload on db6f180 peaked at **66.9571% (STAY)**
over 605.2 seconds (121 windows), with zero underruns and six successful
pattern save/load cycles. It retains the two-source workload below and adds
saved Env 2/3 settings and 64 routes using live sources, including Env 1/3.
The image SHA and complete readback are recorded in
[the performance log](callback-performance-log.md). This permits the per-voice
LFO milestone; it does not close the one-hour phase soak.

The subsequent clean 6ad2896 LFO workload peaked at **72.8827% (REVIEW)**
over 605.2 seconds, with zero underruns. It was superseded by the clean
8730cfd build-profile-edit repeat: **68.0921% (STAY)** peak, 30.7% average,
zero underruns/drops and six file cycles over 605.2 seconds. This permits the
held-note milestone but does not close the one-hour phase soak. The earlier
dirty ITCM placement diagnostic reached 65.3527% peak over 125.035 seconds;
it remains placement evidence only.

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

Requested 2026-09-17: evaluate the [M4 link/storage service](rt1170-migration.md#11-proposed-m4-link-and-storage-service)
as the RT1176 target core split after an M7-only audio baseline. M4 owns link
I/O and SD/FatFs; M7 retains audio state and timing. Resolve shared-memory/cache
handoffs, bounded command latency during SD stalls and measured bus contention
before implementation; this proposal does not close a hardware gate.

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
physical integration and DIN MIDI remain unverified or pending.
The user authorized keypad/LED firmware on 2026-09-17 with a temporary TLC5947
backend and a replaceable interface for a later PCA9956B.
Stages, one commit each:

2. TCA8418 interrupt-driven keypad task implemented, with bounded FIFO drains,
   polling fallback and held-key recovery. Host tests pass; physical wiring,
   shared-touch behavior and latency remain open in HV-011.
3. LED firmware implemented 2026-09-17: `panel_task` owns SPI2/TLC5947 and
   absorbs the PCNT poller. Chip-independent LED policy, diagnostics and console
   status are in place; PCA9956B is an explicit replacement stub. Host tests and
   compile checks do not close the electrical/latency gate in HV-012.
4. MCP3208 + RV112FF 20 kΩ pot firmware implemented 2026-09-17: host-tested
   decoder and calibration service, NVS persistence, Settings → Pots, four pot
   bindings and strip in Instrument Filter/Amp and Play. Shift selects fine
   changes. Pots default disabled until calibrated and saved. Hardware waveform,
   settling, feel and shared-bus/render timing remain open in HV-013.
5. MIDI port output implemented 2026-09-17: bounded per-port queues, UART2
   TX ring, USB event-packet output, existing `MSG_SEQ_CLOCK_OUT` route and
   `MIDIOUT` diagnostics. DIN stays disabled pending receiver wiring confirmation;
   the enabled build compiles. USB input/output flags work independently.
   DIN/USB latency, enumeration and output timing remain unmeasured in HV-014.
   Daisy event generation and external clock/SPP ingest are now implemented;
   end-to-end physical sync remains unverified in HV-014.

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
   both keyboard zone maps, Env 1–3, the eight-row matrix editor and the
   backend runtime/typed transport for two per-voice LFOs are built. The four
   Instrument filter modes (LP, HP, BP and Notch), the per-Instrument filter
   topology (SVF or ladder) and their typed transport are implemented; the resonance matrix destination and OSC1_PITCH/OSC2_PITCH
   destinations are the current implementation checkpoint, while other new
   mod destinations remain open. Instrument sound controls now preview
   automatically with typed filter/amp readback and a per-Track Apply/Revert
   undo point; supported controls now update held voices through the bounded
   handoff, while map/sample replacement retains its stop/next-note boundary.
   The two-row eight-tile
   LFO page adds waveform, frequency, sync, retrigger, pitch-follow, delay and
   fade controls. The held-note milestone is implemented and hardware-verified;
   The held-edit run on 328a419 reached 71.1283% peak and was REVIEW. The
   subsequent clean filter-cache run on 5466a96 reached 69.0852% peak and is
   STAY as a pre-filter-mode baseline. The clean filter-mode run on 1a2eb5e
   reached 69.4887% peak and is STAY as the pre-resonance baseline. The clean
   resonance run on d891d3c reached 69.9237% peak and is STAY as the
   pre-pitch baseline, with zero underruns/drops; it is near the 70% threshold.
   The clean oscillator-pitch capacity checkpoint from 6a58a7b reached 69.9396%
   peak and is STAY, with zero underruns/drops and 16 pitch routes verified.
   It is near the 70% threshold and does not close the one-hour phase soak.
   Each future callback milestone requires a fresh full measurement.
   These runs do not close the one-hour phase soak or permit a voice-count
   change.
3. Bank (stage 6): the [WXCF Bank codec](features/bank-persistence.md) and
   bounded resident index are host-tested. The SD transaction adapter now
   supports named create/copy, a stored/cleared slot, index loading and private
   Instrument decoding, with free-space admission and failure cleanup. It is
   host-tested/compile-verified, without a device entry point yet. SD working-copy management, Bank
   Manager, Track recall, preload and Program Change recall remain open and
   follow the expanded Instrument engine. The requested Bank Manager must
   create/name/open/save Banks, browse stable numbered slots, store an edited
   Instrument explicitly, and recall a slot into the selected Track. Add
   copy/move/clear with explicit replacement and unsaved-edit behavior; preserve
   stable slot identity and validate sample admission before replacing a Track.
4. Polyphony policy (stage 8), from stage 5's measurement.
5. Finish [Mixer v1](features/output-routing-and-mixer.md): Performance now
   replaces the Track root and exposes Instrument assignment, MIDI input,
   Track level and pan across the existing eight-Track selector pages.
   Correlated mixer target readback is implemented. Complete the full strip
   view with mute/solo, audible master controls and subscribed per-Track meters. Decide visible strip count/paging against the
   touch budget. Keep Track mix independent of Instrument sound trim; verify
   authoritative readback, solo/mute restoration, click-free changes and
   bounded redraw/telemetry cost during playback, then run the hardware soak.
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

Requested character operation: [vintage sampler grit](architecture-notes.md#vintage-sampler-grit)
for layered one-shots — 27.7/22.05 kHz, companded 8-bit quantization,
rate-based pitch, pre/post filtering and gentle saturation after the filter.
Follow the basic render pipeline to prepare reduced-rate assets. The requested
[playback model keeps the engine at 48 kHz](features/vintage-sampler-math.md)
and simulates per-voice sample clocks; complete offline prints are optional.
Runtime reconstruction remains unscheduled pending a separate callback-capacity
gate; the numerical reference does not establish device performance.

**Gate:** record, edit, slice, assign, and sequence entirely on-device while
playback remains uninterrupted.

## Phase 5 — Performance and polish

Extend song/performance workflows with scenes/macros, send effects, tuning/scales, Instrument/Bank
browser quality (Bank Select, "save with samples"), settings persistence, USB
sample import, and a measured CPU/memory headroom pass. Consider polyphase sample-rate conversion only after profiling
shows linear interpolation is a meaningful cost or quality limit.

## Outstanding hardware verification

- **Performance setup (2026-09-14):** both firmware builds and host checks
  cover the renamed Track root, selected-Track gain/pan and correlated
  mixer readback. Verify assignment/replacement/cancel and selected-Track
  continuity; level/pan during held notes and sequencing; Instrument replacement
  preserving mix; link loss/reconnect; and idle/local-control repaint cost on
  the panel. No rendering, click-free or audio-latency claim is established
  by compilation. The full Mixer and phase soak gates remain open.

The following code paths are open until observed on the target:

| Area | Verification |
|---|---|
| Multi-touch | Hold Shift and press an alternate softkey; drag two different value tiles/dials; lift either finger first and re-touch while the other keeps moving. Check two Play pads release independently, navigation while held, five contacts, corner coordinates and wake from blanking. Host pointer tests and compilation do not establish GT911/panel behavior or touch/audio latency. |
| Display rotation/PPA | Check the HX8394 panel for corruption or tearing. |
| Partition migration | Flash, boot, and confirm settings persist. |
| Audio formats | Audition 44.1 and 48 kHz WAVs; confirm pitch. |
| UART and SD | Sustain traffic during streaming; run read and hot-unmount soak tests. |
| MIDI latency | Measure DIN and USB input-to-sound latency; target under 5 ms. USB MIDI enumerates on the USB 2.0 HS controller — the board's 4-pin USB connector, not the Type-C — and that has never been confirmed on the bench. DIN waits on 2.P.5 (receiver on the new RX pin). |
| Panel pins (2026-09-05) | `pin_config.h` was rewritten against the ESP32-P4-WIFI6 header. The bench encoder is PCNT unit 1 (confirmed 2026-09-05); it counts negative on clockwise as wired, and three pages had compensated for it — direction is now one per-encoder flag in `hardware_config.h`, and those pages follow the shared contract. Clockwise increases values / moves forward on every page — verified 2026-09-05. Verify the TCA8418 matrix geometry (`WAVEX_TCA8418_ROWS/COLUMNS`, never confirmed against the wiring) and the `WAVEX_KEYCODE_*` map from the Diagnostics ▸ Panel tab (2.P.1): press each key, read its keycode, row/column and `PanelKey`; "unmapped" means the map or the geometry is wrong. Blocker first: the bench log shows `TCA8418 hardware initialization failed` on every boot recorded (2026-09-05), so the keypad has not been answering on I2C at all — check its wiring and address before reading anything off the Panel tab. Scope an endless pot's two wipers before calibrating (the decoder assumes triangle waves). |
| Diagnostics | Open the page and verify live telemetry arrives. |
| Digital voices | Trigger RAM-resident notes, sweep live parameters, and judge SVF response/resonance. |
| Ladder filter (resolved 2026-09-14) | Three ladders were measured on one workload (`callback-performance-log.md` § Filter topology A/B): Huovilainen 4x peak 69.8%, 2x 47.5%, ZDF 33.7%, SVF 30.3%. Listened to on the bench they were the same filter at working settings (-35 dB difference); the ZDF was kept as `Ladder` (about 3 points of budget for eight voices) and the Huovilainen ladders were pruned, wire values 2 and 3 retired. Still open by ear: the ladder's self-oscillation (from 74% RES) and the SVF's Q 16 top with the reworked drive, on `scripts/bench_filter_listen.py`. Related, on the gate backlog: a held pad cannot keep a one-shot voice open past its sample end, so a pluck's ring only lasts its amp release. |
| Sample retirement | Four-Track routing, rebinds and SFZ replacement during sequencing, and sample-edit refresh now have passing HIL coverage. Still exercise delayed/stopped callbacks and concurrent import requests. Confirm timeout preserves storage, then measure DWT headroom and run the zero-underrun soak. Host helper tests do not verify this interrupt integration. |
| Callback budget | The repeated post-ITCM eight-voice workload remains STAY; all four DSP candidates were retested and only modulation exponent caching was adopted. See `callback-performance-log.md` for the current reference and rejected trials. Render/event/modulation attribution and selective placement A/B are implemented and measured. Remaining work includes the one-hour soak, fresh gates for expanded callback features and any further placement/compiler experiments. The historical DaisySP comparison remains default-disabled and needs its own updated capacity evidence. |
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
- Track physical checks and their evidence in
  [hardware-validation.md](hardware-validation.md); leave deferred checks open
  and link them from their owning gate here.
- New subsystems need a focused feature design before implementation.
- Delete superseded docs after moving remaining open work into this document;
  git history is the archive.

## Next steps and backlog

These items are part of this roadmap. They retain their phase dependencies;
listing an idea does not mark its design accepted or hardware verified.
Completed work belongs in the changelog and git history. This section absorbs
the former separate backlog (2026-09-13).

### Composition and performance workflows

The [Project, menus, mixing and voice-channel model](features/project-menu-and-voice-model.md)
consolidates the 2026-09-14 discussion, including current behavior, requested
stereo/Mono changes and the proposed Project navigation and Scene workflow.
Project exposes Track assignment, MIDI input, level, pan/balance and mute;
it owns that setup without needing a separate saved Performance object.

- [ ] **Stereo/Mono hardware verification:** remaining physical checks are in
  [HV-004](hardware-validation.md#hv-004--stereo-and-mono-physical-checks) and
  [HV-005](hardware-validation.md#hv-005--phase-2-timing-and-soak-gate). Stereo
  preservation, saved per-oscillator Mono (default Off) and the render-channel budget
  are implemented. Host tests cover mixed-cost stealing, release tails,
  independent stereo filters, next-note/undo behavior, old WXI headers and a
  larger configured capacity. Ten stereo/Project HIL cases now pass (2026-09-16):
  all five full-budget mixes, whole-note stealing (including two Mono notes
  evicted by one stereo note), held-note/next-note Mono,
  Project level/pan/mute, Apply/Revert, saved per-oscillator Mono and manual
  mutes across Solo. These are debug-console and digital-meter checks;
  audible stereo/pan quality and physical controls remain open.
  [Stereo DWT captures](callback-performance-log.md#stereo-channel-verification--2026-09-16)
  record the new renderer separately from earlier mono measurements. Still
  run a matched before/after comparison and the full channel-budget soak
  with streaming audition and modulation. Sample Edit `channel_mode` UI
  remains separate. WXI recall fixtures must meet Instrument import admission;
  direct-loaded large samples can exceed that limit (see below).
- [ ] **Phase 2 / Waveform playback head:** software implemented for the current
  forward RAM voices and streamed audition, shared by Browser, Sample Edit and
  Record waveform panels. A source-frame line follows rate, retriggers, loops
  and the newest matching sample voice; matching streaming audition takes
  precedence. Zoom clips the line; loop splice views omit it. See the
  [as-built design](features/waveform-playback-head.md). Host tests cover
  identity/expiry, rate mapping and narrow LVGL redraws. Panel tracking, UART
  load and matched callback/zero-underrun measurements remain open in
  [HV-010](hardware-validation.md#hv-010--waveform-playback-head). Reverse and
  Track/Zone-specific cursors remain tied to future playback/context support.
- [ ] **Instrument/Kit polyphony policy (Phase 2.5 follow-up, measure first):**
  implement the requested [saved allocation defaults](features/project-menu-and-voice-model.md#instrument-and-kit-allocation-policy)
  for Mono, Auto/1–8-note caps and Own only / Own first / Any stealing, with
  explicit Track inheritance/overrides. Kit-wide plus per-pad refinements remain
  proposed. Define note-group identity, transactional multi-layer/stereo
  admission, note-off/retrigger behavior and old-file defaults before changing
  the allocator. Complete persistence/protocol tests, Apply/Revert and UI, then
  audible-steal and DWT gates; this does not raise the global channel budget.
- [ ] **Phase 2 / Instrument save admission bench check:** runnable checks are in
  [HV-003](hardware-validation.md#hv-003--instrument-save-and-recall-admission).
  Save copy preflights both oscillator maps' on-card WAV dependencies under recall's
  format and per-sample size policy before creating a WXI. Host regressions
  cover rejection and preserved ownership/undo state. Include this preflight in Project
  snapshot transactions; session-wide memory admission remains separate.
- [ ] **Phase 2 / Card storage bench check:** all implemented saves now
  preflight free space; Settings > Storage provides confirmed formatting and
  creates the expected directories. Procedures/results are tracked in
  [HV-001](hardware-validation.md#hv-001--sd-card-formatting) (formatting deferred
  by user) and [HV-002](hardware-validation.md#hv-002--save-free-space-checks).
  Host/compile verification does not close this bench gate.
- [ ] **Phase 2 / Mixer v1:** Project now exposes Track assignment, MIDI, level,
  pan/balance and mute. Add session Save/Load; preserve selected-Track continuity and keep Pattern/
  Song editing in Sequencer. Complete Project transactions and mixer readback/
  mute/solo/master/meters at their existing phase gates.
- [ ] **Phase 5 Scenes:** settle the proposed eight project-scoped snapshots,
  explicit Store/Update, initial level/pan/mute contents, quantized manual recall
  and optional Scene references on Song entries. Resolve automation, stop/seek,
  deletion and Pattern authority before versioned storage and atomic recall.
  Macros and effect sends follow their own available runtime controls.

### Sampler and synth architecture references

- [ ] Evaluate the [ASR-10 and E4/EOS architecture notes](architecture-notes.md) when
  revisiting Instrument navigation and performance workflows: visible edit
  scope, optional select-by-playing, resident sound variations and
  velocity-shaped articulation; E4 adds group edit scope, shared multisample
  sound definitions, resource-aware loading and bounded control processors.
  The [Cord follow-up](architecture-notes.md#eos-cords-further-modulation-lessons)
  captures route-depth control, reusable mixes, source lifetimes and distinct
  trigger/evaluation rules. These are exploratory ideas, not accepted engine
  or menu changes. Apply context/navigation lessons within existing
  Phase 2/2.5 work; recall and offline commands retain their Phase 5 and Phase 4
  placement. New voice features remain unscheduled and require an ownership
  design plus the callback capacity gate before implementation.

- [ ] Resolve the [workflow coverage gaps](architecture-notes.md#workflow-coverage-and-remaining-design-gaps)
  alongside their existing phase tasks: device/Project settings ownership,
  sample-versus-Zone edit scope, reference-aware sample actions, portable
  collection/missing-asset repair, bounded undo and record/arrangement behavior.
  The notes distinguish existing plans from new candidates; they do not expand
  the current phase gate or replace the sample-operation and persistence tasks.

### Oscillator drift — unscheduled

Requested 2026-09-14; **backlog only**. See the
[drift proposal](features/project-menu-and-voice-model.md#oscillator-drift-backlog):
bounded pitch variation in cents, independently per voice/oscillator, using
note-on random offsets and/or slow continuous drift. Default amount zero;
stereo pairs share one pitch trajectory. Decide Instrument-wide versus
per-oscillator amount and any global scale, rate/distribution, retrigger and
combined bounds before implementation. Reuse the existing pitch modulation
path and measure callback cost; this is not part of the current stereo patch.

### Firmware audit remediation — 2026-09-06

This is the active task list for the audit of first-party
`firmware/esp32`, `firmware/daisy`, and `firmware/shared`. Vendor code is
checked at the API/DMA boundaries used by WaveX. Priorities describe concrete
failure modes; hardware-only gates stay in the roadmap. Remove each task when
its fix and regression checks are committed.

- [ ] **Medium — remaining reference consistency.** Finish checking the
  platform and feature guides against live transport and parameter behavior;
  the navigation, sequencer and testing references have been consolidated.
  Keep hardware-only results separate from host coverage (principles 13, 15).

### Note lengths and one-shot playback — decision pending

Deferred at the user’s request (2026-09-13), linked to the existing
[Phase 2.5 melodic sequencing design](features/melodic-sequencing.md).
Revisit that draft before implementing its gate/tie model. Decide how sequence
note duration relates to one-shot versus gated Instrument/zone playback;
release tails, overlapping repeated notes, chord lanes, tempo changes and
transport-stop cleanup need explicit behavior. Keep note-off scheduling on the
Daisy audio clock with bounded storage and stable voice/note identity.
The [MPC1000 manual](https://cdn.inmusicbrands.com/akai/mpc1000/mpc1000v2_operators_manual_00.pdf_055b8b24eeb101e27a2f4042c6819727.pdf)
(printed pages 32, 36 and 67) is a reference: sequence duration and one-shot
versus note-on playback are separate choices, not necessarily Track types.

The drum-and-chord UART benchmark may use explicitly bounded, unlooped sample
regions at its fixed tempo. Those one-shot regions are a test arrangement,
not an implementation of sequenced note lengths or MIDI note-off gates.

### Performance, build, and transport

#### Callback capacity checkpoint

The accepted post-ITCM WaveX reference and the four DSP retests are recorded in
[callback-performance-log.md](callback-performance-log.md). The unchanged ITCM
image has three full baseline captures; modulation exponent caching was then
adopted after two confirming runs, and the other three candidates were rejected.
The current capacity band is STAY, with the one-hour soak and complete Phase 2
gate still open.

Render, event and modulation attribution and selective hot-path placement have
been measured. Remaining work is to remeasure every callback expansion and
complete the full soak. Further optimizations are deferred at the user's request
(2026-09-13):

- Benchmark the remaining QSPI math helpers using the selective placement
  procedure below; keep the accepted compiler/profiler and workload fixed.
- Split event timing into note triggers, parameter locks and live snapshot
  application. Move eligible preparation outside the callback only after
  attributing the bursts; preserve sample-offset timing and immutable handoffs.
- Investigate UART TX queue-full bursts and repeated foreground polling.
  Consider coalescing replaceable telemetry and avoiding unnecessary service
  work. Queue-full counts include rejected enqueue attempts that callers may
  retry; distinguish them from lost musical events. Foreground UART elapsed
  timing includes preempting audio interrupts and does not establish that UART
  caused a callback peak.

The historical DaisySP comparison exceeded the capacity threshold; it remains
default-disabled and needs fresh evidence on the expanded workload before
adoption. The rejected cutoff/setup caches, specialized rendering loops and
global O3 trial remain rejected unless a new controlled measurement justifies
revisiting them. This does not authorize a backend port or hardware purchase.

#### UI responsiveness follow-up

The measured Sequencer, Play, Instrument and Diagnostics fixes and their
hardware limits are recorded in [UI latency notes](ui-latency-notes.md).
Shared value-tile/dial/chrome changes also apply to their other callers;
that is not per-page timing evidence for Settings, Track, Pad Map, Pad Sound
or Sample Edit. Profile those interactions when changing the relevant page,
including encoder/touch bursts and concurrent loading. Physical GT911-to-panel
latency and tearing checks remain open; synthetic touch/readback and renderer
counters do not close them. Follow the project LVGL skill when building new
pages so unchanged snapshots and local edits do not repaint the whole screen.

#### Page-entry render cost

Sample Edit, Sample Record, and Play have expensive first frames (up to about
205 ms for Sample Edit) despite inexpensive steady-state rendering. Profile a
smaller or filled waveform representation before changing the widget, and
spread page construction across frames only if rendering cannot meet the
interaction target.

#### Daisy optimization and LTO

The Daisy image retains `-O2`, with the profiling implementation pinned to
`-O2` for comparable trials. The measured global `-O3` experiment was
slower and was not adopted; see
[callback-performance-log.md](callback-performance-log.md). LTO remains a
separate measured experiment because it can affect linker placement and weak
HAL symbols. Establish a repeated reference and hold instrumentation fixed
before accepting another compiler configuration.

#### Profile-guided QSPI-to-SRAM execution

The persistent QSPI image uses selective relocation:
`WAVEX_ITCM_CODE` gives code a QSPI load address and an ITCM run address, and
`MemorySections::InitItcm()` copies it before interrupts start. The UART
RX-position handler, voice rendering/events, LFO setup, callback and modulation
evaluation now use measured ITCM placement. Hot voice state remains in DTCM.
The accepted repeated baseline and four subsequent optimization trials are in
[callback-performance-log.md](callback-performance-log.md).

The accepted profiling ELF still places `powf`, `tanf` and
`arm_sin_f32` in QSPI. Further selective relocation remains a benchmark
candidate, with before/after DWT evidence and memory-map checks required.

A matched `-O0`, profiling-enabled bench on 2026-09-04 streamed the same
44.1-kHz stereo WAV from SD in both profiles. The active audio callback
averaged 52.39 us from QSPI and 17.84 us from SRAM; the foreground WAV pump
averaged 1.416 ms and 1.189 ms respectively, with no observed underruns. The
current release-optimized eight-voice evidence is recorded in
[callback-performance-log.md](callback-performance-log.md). The old `-O0`
comparison is diagnostic history, not the reference for current optimizations.

If selective ITCM placement is insufficient, evaluate a separate persistent
bootloader-SRAM profile. libDaisy's `BOOT_SRAM` model stores the application in
QSPI and copies it into SRAM at boot, so runtime should resemble the direct-SWD
SRAM profile while surviving power cycles. Do not replace the existing QSPI
execution profile unless the image remains below the bootloader's 480 KiB
limit, D1 heap and the currently tight D2/D3 regions retain measured headroom,
and an identical eight-voice plus SD-streaming soak shows a worthwhile DWT
improvement with zero underruns. Record flash time, boot-to-audio latency, map
usage, and callback min/average/max for both profiles.

#### SPI-link revival requires hardware verification

Investigate the recorded pattern-save SD error under the 24 MHz slew trial
(`fatfs=1`, `hal_err=0x6`, offset 5120) and its recurrence with fast scheduling
(offset 0), including repeat/soak coverage and
whether combined electrical activity contributes. SPI invalid-frame/timeout
counters stayed zero; do not treat that as a complete system pass.

The cutover measured slower acknowledged controls on SPI despite faster bulk
wire time. Fast scheduling now removes the five-millisecond interval and
gives deferred replies a foreground pass before another empty frame.
Before another adoption comparison, measure the remaining immutable
empty-frame delay, fixed-slot padding and queue backpressure. Preserve DMA ownership and measure control latency and audio
load together. Treat selective SPI ITCM placement as a measured candidate; it
does not establish signal integrity. The 48 MHz trial failed on Daisy RX
with both fast and legacy cadence. The original nominal 32 MHz midpoint
failed too, but matched mode 1 plus stronger ESP MISO drive later completed
two 200-edit control trials and four selected two-board HIL checks without
reported link errors. Mode or drive changes alone did not pass; 48 MHz still failed and its captured RX prefixes were
exactly one bit late. Validate SCLK/MISO timing at the Daisy receiver and
repeat/soak the working combination before increasing the clock further.
Finer rate candidates require a separately validated boot-time clock
profile; short trials do not establish a stability threshold. See the
[return-path investigation](spi-notes.md#return-path-investigation).

UART remains the default and production transport. The September 2026 audit
fixes descriptor ownership, duplex RX publication, parser capacity,
sequence/length handling, READY signaling and bounded recovery.

The requested reversible experiment is implemented on
`experiment/mcu-link-switch`: the shared selector controls both MCUs'
initialization, startup, routing, sends and service, with DMA derived from it.
Only the selected transport carries application traffic. The Daisy TX-only
pump retains RX in a bounded queue without recursive command dispatch. SPI
uses the existing exact-length codec to serve strict application handlers.
Both builds print their selection, and initialization failure stays offline.

Both selections compile and the repository host suites pass. The SPI transport
tests also pass with ASan/UBSan. The UART/SPI/UART hardware comparison and its
limits are recorded in [SPI notes](spi-notes.md#verification-and-remaining-gates).
Use [flashing](flashing.md#uartspi-comparison) for the one-macro rebuild/flash
procedure and retained-image rollback. UART stays the checked-in default.

The remaining hardware gate includes fault injection and peer-reboot recovery.
Define application retry behavior for CRC rejection and a peer that never
reasserts READY. The Daisy recovery path requires exclusive ownership of
libDaisy's SPI DMA streams; Stage A has no competing general-purpose SPI DMA
consumer, while Stage B CV sharing must be resolved before combining them.

Use [spi-notes.md](spi-notes.md#verification-and-remaining-gates) for evidence,
limits and the bench gate. The GPIO continuity test passed; high-speed signal
integrity, fault recovery and the full soak remain unverified despite the
targeted corrected-DMA bench checks.

#### Streaming CRC recovery

SD CRC recovery can block the main loop longer than the audio ring lasts.
Choose and implement bounded recovery behavior: pause and recover, abort the
stream, or increase the prebuffer. Validate the choice with injected or
reproducible CRC faults and capture ring low-water and service latency.

#### Daisy build warnings worth acting on (2026-09-06)

The release build is not warning-clean, and two of the warnings are real:

- `PumpSampleMetaPage`: the page's byte count is a `size_t` narrowed to
  `UartLinkSend`'s `uint16_t` (`-Wconversion`). In range today (one header
  plus at most a page of records); make the narrowing explicit with a bound
  so a wider record cannot wrap it.
- `sfz_loader.cpp`: `FF_USE_LFN` tested with `#if` where it is not defined
  (`-Wundef`, twice) and an enum/int mix in a conditional (`-Wextra`).

#### The SRAM debug profile has ~7 KB of RAM_D2 headroom

`make daisy-debug` / `make flash-fast` link the backend to run from SRAM with
its data in `RAM_D2` (256 KB). At `eb15470` (the path widening) that link
failed — `RAM_D2` overflowed by 9532 bytes — so the fast bench loop could not
load the Daisy at all and only the DFU path (`make daisy-flash-auto`) worked.
Retiring the decimated preview (`6f53720`) freed ~16 KB and it links again at
97.4% (255 276 B), which leaves about 6.9 KB. The next resident buffer of
that size breaks the debug profile before it troubles the release one; the
`.wxi` document buffer move in [the SRAM item](#the-daisys-sram-is-at-89-and-the-wxi-document-buffer-is-the-lever)
is the lever for both. `make flash-fast` should report the Daisy link
failure by name — today it prints the ESP32's success and exits 1.

### Memory

#### The Daisy's SRAM is at 89%, and the .wxi document buffer is the lever

The 2026-09-06 path widening took internal SRAM from ~84% to 89.1%
(467 KB of 512 KB, ~57 KB free); retiring the decimated preview the same day
(protocol 3) gave ~16 KB of that back (its 4096-point buffer and frame
staging), for 86.0%. The single largest new consumer is the loader's `.wxi`
document buffer (`s_doc_storage`, `sfz_loader.cpp`): a `Wxi::InstrumentFile`
is ~17 KB once each of its 64 zone slots carries a 256-byte path.

It does not belong in SRAM. It is a main-loop working buffer, written once per
load and never touched by the audio callback — the same profile as the Sample
Pool's records, which already live in SDRAM. Moving it recovers ~17 KB.

**Do not reach for `__attribute__((section(".sdram_bss")))` to do it.** That
section exists in both linker scripts and starts at the SDRAM origin, which is
exactly where `sdram_layout.h` puts the *sample arena* (`kBase = 0xC0000000`).
The layout reserves nothing for linker-placed SDRAM data, so anything landing
there silently overlaps user sample memory. Carve the buffer from a partition
the layout owns — the render scratch is unused and adjacent — or extend the
layout to reserve a linker-placed region first.

When to revisit: before the next feature that adds a large resident buffer, or
if a build reports SRAM above ~92%.

### UI and frontend maintenance

#### Play pitch labels

The final UI screenshots show C4 on the first Pad while the Octave tile reads
5, and Keys retains the Pads-sized header range. Reconcile the display octave
convention and active-surface range label without changing MIDI note numbers.
This is presentation follow-up, separate from note-length/gate semantics.

#### Touch coordinate verification

The GT911 and rotated 1280×720 LVGL canvas may use different coordinate
orientations. Corner-tap the actual panel before changing driver configuration;
do not guess an axis swap in the vendored BSP.

#### Break the `components/ui` ⇄ `main` dependency cycle

Pages call frontend globals directly, so the UI component cannot be host-tested
independently. Inject a narrow `UISharedContext` through the navigator, move
`ICommInterface` out of `main`, then remove the `main` CMake requirement. Do
this after the current UI hardware pass, not alongside unverified behavior.

#### Busy-overlay errors

Errors currently use an in-flight spinner and can be rewritten by its timeout.
Add a non-spinning `showError()` path with stable text and an intentional
dismissal policy.

#### Encoder counter race

The PCNT read-then-clear sequence can lose counts. The current re-centering
makes it rare; close the race only with an IRAM-safe watch-point design or by
moving consumption into the UI task.

#### Unused frontend code

In a separate deletion pass, remove the caller-less ESP32 APIs, unused window
manager, and unreachable UI surfaces after re-checking callers.

### Samples, instruments, and browsing

#### Non-frame-aligned WAV data

Some legal WAV files whose `data` payload begins off a frame boundary produce
artifacts. Reproduce with a minimal fixture and identify the parser or stream
path fault; do not round the offset to a frame boundary without proof.

#### Resident-sample status capacity

The status message and ESP32 metadata cache cover eight entries while the
Daisy can hold 32. Before kits or multisampled instruments depend on it, add
paged status or an explicit sample-unloaded message so the frontend can
reliably remove stale rows.

#### Browse paging

Directory listings stop at 256 entries. Implement paging or a bounded cache
when the 500-entry browse target becomes a product requirement; increasing a
static array is not the solution.

#### Sample operations

There is no protocol operation to unload, rename, delete, or reorder a loaded
sample. Define one versioned `MSG_SAMPLE_OP` verb in a reserved protocol block,
with round-trip tests, after its effects on instrument references are defined.

#### Wavetable oscillator source

The architecture now reserves a typed oscillator-source boundary so sampler and
wavetable engines can share Instrument/Track/Voice ownership without pretending
that a wavetable is a short looping sample. Its place is fixed: one of an
Instrument's two `Oscillator` slots, `OscType::Wavetable`, with `WT_POS` as its
mod destination ([track-and-patch-model.md](features/track-and-patch-model.md)
§3.1). The wavetable renderer is an unscheduled, post-Phase-2.5 candidate;
sampler reliability, Instrument persistence, sequencing, and the zero-underrun
gates come first.

Before scheduling in a phase, resolve the implementation decisions listed in
[oscillator-sources.md](features/oscillator-sources.md): import metadata,
cycle/frame limits, interpolation and anti-aliasing, modulation behavior, WXCF
chunks, RAM admission, and a measured DWT budget. Any earlier refactor may only
extract the typed source seam as a small, behavior-preserving change needed by
scheduled sampler work.

#### Oscillator sync and FM

Requested 2026-09-12. Unscheduled, post-Phase-2.5 candidate for an
Instrument-owned interaction between its two oscillator slots. Before any
implementation, decide master/slave direction; hard versus soft sync for
sample and loop sources; true-frequency versus phase modulation; routing,
depth and feedback policy; alias suppression; source compatibility; saved
settings; and the measured callback budget. Sampler reliability, persistence
and current audio gates come first. This item does not define a protocol or
enable audio behavior.

#### Parameter-id design drift

The target parameter-lock document reserves IDs that conflict with live pan and
pitch values. Reconcile the design with `protocol.h` before implementing
parameter locks or modulation; do not renumber live wire values without a
protocol migration.

### Hardware, diagnostics, and logging

#### ESP32 pin verification

`pin_config.h` was reconciled against the ESP32-P4-WIFI6 header on 2026-09-05
(the SPI2/PCNT collision is gone; see `features/panel-controls.md`). What is
left is bench work, tracked in `roadmap.md` § Outstanding hardware
verification: which encoder is physically wired and to what, and the keypad
matrix geometry. The dormant SPI-slave link's five pins stay reserved until
the SPI revival decision above is made; releasing them for the panel is the
alternative if that decision is "never".

#### Logging policy

Fold legacy `UART_LOGx` and Daisy compatibility call sites into the module
table incrementally. Decide the release log ceiling from field-diagnostics
needs, not the roughly 4 KB text saving at WARN alone.

#### Backend link instrumentation

`WAVEX_DAISY_UART_PERF_DEBUG` gates backend link counters. Measure its
hot-path cost before deciding whether cheap counts should remain enabled in
release and whether the flag follows the build profile.

#### Filter: a third topology and cheaper retunes

`audio/voice_filter.hpp` renders each voice through the topology its
Instrument selects (`InstrumentFilter::topology`, on the wire and in WXI
since 2026-09-13): the WaveX TPT SVF or the DaisySP Huovilainen ladder.
Slope and drive joined it as Instrument parameters on 2026-09-14.

- **Third topology.** Candidates, all permissively licensed: a Korg35 or
  diode ladder from Faust's `vaeffects.lib` (STK-4.3, MIT-style; Will
  Pirkle's designs as transcribed by Eric Tarr) generated as checked-in C++,
  or a first-party TPT implementation of the same designs. Blocked on the
  ladder's eight-voice DWT number (Outstanding hardware verification): a
  second unmeasured filter on top of an unmeasured one is not a decision.
  When it is added, fold the inactive topologies' state into a union inside
  `VoiceFilter`: every Voice carries every implementation's state today,
  about 160 B of DTCM each, fine for two and wasteful for four.
- **Stilson ladder as the ladder fallback.** If the DaisySP ladder fails
  its eight-voice gate, the Stilson model (public domain / Unlicense in the
  ddiakopoulos `MoogLadders` collection) is the cheap "character ladder":
  no oversampling, a single soft saturation, cheap coefficient updates. Its
  known limits are resonance/cutoff coupling, instability at extreme
  resonance and no true self-oscillation, which is why it is the fallback
  and not the first choice. It would slot in as a third `FilterTopology`
  value or replace `Ladder` outright, decided by the measurement.

#### Daisy image size: what is left after the 2026-09-04 slimming

The SRAM debug image (`make debug-build`) stopped linking between 5998d37
and 385f93e (about 10 KB over the 480 KB SRAM region at `-O0`, found
2026-09-13). Fixed the same day by building the vendor archives `-Os` in that
profile only (`WAVEX_SRAM_DEBUG_VENDOR_OPT`); WaveX sources stay `-O0`, and
the image now has roughly 60 KB of SRAM headroom. The spill lists in
`wavex_sram_debug.lds` were not touched. If it overflows again, the next
levers are `DEBUG_OPT=-Og` for WaveX sources or spilling more `.bss` into D3
(about 30 KB free) - both before anything that assumes the debugger can
load DTCM or ITCM directly, which has not been verified on this bench.

The Daisy image went from 438 KB to 273 KB. Remaining, in order of size, each
a decision rather than a mechanical fix:

- ~~`WAVEX_DAISY_OPT` is still `-O0`~~ Done 2026-09-04: `-O2` by default
  (image 275 772 to 189 636 bytes). The whole-callback `-O2` soak is in
  [callback-performance-log.md](callback-performance-log.md); `Render()`
  timing and `-O3` still need a reasoned comparison.
- **USB CDC logging (`hw.StartLog`) is in every profile**, ~19 KB flash and
  13 KB SRAM including `stm32h7xx_ll_usb.c`. Gate it on `WAVEX_BUILD_DEBUG`
  once field logging is confirmed to go via the UART bridge; libDaisy's
  `usbd_core.c` also carries the only remaining `printf` callers (now routed
  to the log ring).
- ~~Stack temporaries of the `obj = T{}` form~~ and ~~`-Wformat-truncation`
  at `-Os`~~ both done 2026-09-04 (75145f3).
- **`HAL_HCD_IRQHandler` + `hhcd_USB_OTG_HS` (~2 KB)** stay linked through
  libDaisy's own `system.cpp` IRQ table; removable only by a submodule change.

#### Diagnostics coverage

The UI screenshots also show four-digit resident Sample IDs wrapping in the
narrow ID column. Rebalance the existing table column widths for the full ID
range and verify readability with an occupied Sample Pool.


Add active-voice and round-trip-latency telemetry when it has an owning
protocol change. Add MIDI detail and dropped-event counters with the Phase 2
sequencer and tempo-follower work, when those events actually exist.

The Link tab's `unknown` count is meant to read 0 in a healthy session, so a
non-zero value means corruption or a protocol mismatch. It does not yet: the
backend answers every audition-by-path with a `MSG_ACK` (`serial_id` 0,
`ProcessSamplePlayRequest`) that the frontend neither routes nor counts as
known, so each audition adds one. Either drop that ACK - nothing waits for
it - or route it; do not just add it to the known list, which would hide a
message the frontend does nothing with.

#### Root-menu context lines have no data source

The refreshed root menu (design turn 2e) gives every row a live context
readout saying what it currently points at. Only two could be wired
truthfully: Play shows the selected Track, Diagnostics shows link health from
the backend heartbeat. Two more are specified by the design and are blank:

- **Sample — resident count.** No API exposes how many samples are in the
  backend's sample RAM. The number exists in `SampleMemStatusMessage`, which
  `ui_diagnostics_page.cpp` decodes into its own widgets and does not publish.
- **Instrument — instrument name.** `instrument_name_` is a private member of
  `UIInstrumentPage`, so nothing outside that page can read it.

Both want a small shared accessor of the same shape as `current_track.h` /
`current_sample.h` rather than a second copy of the state. Until then the rows
show no context, which is the honest rendering - a placeholder in the root
menu would have to be opened to find out whether to believe it.

#### mocks/ui_theme.h is dead

Nine UI sources include the theme as `"../styles/ui_theme.h"`, which bypasses
`firmware/esp32/tests/mocks/ui_theme.h` entirely - the host test build
compiles the real header and always has. The mock is stale and unused;
either delete it or change those includes to `"ui_theme.h"` so the mock is
actually what the host build sees. Left alone for now because changing it
mid-redesign would swap the palette the host build compiles against.

## Related

- [Architecture](architecture.md) — system ownership and real-time constraints.
- [UI architecture](ui-architecture.md) — current navigation and page lifecycle.
- [Changelog](../CHANGELOG.md) — completed work.
