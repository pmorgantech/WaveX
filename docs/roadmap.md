# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-18.

This document tracks remaining implementation, open decisions and phase gates.
Completed work belongs in [CHANGELOG.md](../CHANGELOG.md) and git history;
bench procedures and results belong in [hardware-validation.md](hardware-validation.md).
Code-complete features stay open there until their physical checks pass.

Phase 0/1 are retired as separate planning sections. Their unfinished concurrent
streaming, recording, platform maintenance and soak requirements are retained
below; retiring the sections does not certify their hardware gates. Phase numbers
remain stable, with deferred Phase 3 moved to the end.

**Next software work:** Bank preload and MIDI Program Change recall, then the
remaining Phase 2.5 tasks in order. The authorized touchscreen/software work can continue
while panel wiring is pending; the full Phase 2 gate remains open.

## Contents

- [Phase 1.5: sample editing](#phase-15--sample-editing)
- [Phase 2: sequencer and pads](#phase-2--groovebox-core-sequencer-and-pads)
- [Phase 2.5: sampler instrument layer](#phase-25--sampler-instrument-layer)
- [Phase 4: offline editing](#phase-4--offline-editing-and-mangling)
- [Phase 5: performance and polish](#phase-5--performance-and-polish)
- [Hardware verification](#outstanding-hardware-verification)
- [Rules for every phase](#rules-for-every-phase)
- [Next steps and backlog](#next-steps-and-backlog)
- [Phase 3: analog board, deferred](#phase-3--analog-voice-board-deferred)

## Phase 1.5 — Sample editing

1. Persist standalone marker/gain edits with the WXCF sidecar model; settle
   Save As naming and sidecar versus render-to-new-file behavior. Project
   snapshots already retain sample edits.
2. Define partial-load behavior for samples too large for resident RAM.
   Explicit resident-sample selection is implemented; panel checks are HV-017.
3. Finish marker interaction: loop seam verification, zero-crossing snap
   protocol/policy for stereo, and playback-time loop crossfade.
4. Reconcile streaming/RAM channel behavior and expose `channel_mode` in the
   editor, including a label for one-channel views.

**Gate:** edit and audition a multi-minute WAV, save, reboot, reload, and hear
that region without a UI freeze.

## Phase 2 — Groovebox core: sequencer and pads

Sequencer/grid editing, voice-scoped locks, Pattern slots/files, Songs, Project
save/load, Mixer, stereo/Mono switching, waveform playback heads and MIDI
clock/SPP now have implementations. Their remaining work is physical validation,
not rebuilding those features. Melodic notes and live recording belong to 2.5.

1. Verify sample-offset timing, edit boundaries and prepared note/velocity
   resolution for the four-Track gate (HV-005).
2. Validate persistence/reboot/failure recovery and card storage, Mixer,
   stereo/Mono, waveform feedback and LFO controls at their existing checklist
   entries. Selected save/load HIL cases pass; the full gates remain open.
3. Complete panel integration and MIDI synchronization below.
4. Complete the callback capacity and one-hour soak requirements below.

### 2.C — Callback capacity checkpoint

Use [callback-performance-log.md](callback-performance-log.md) for measured
images, workloads, repeated captures and rejected experiments. Prior ten-minute
captures do not close the one-hour soak or establish capacity for later features.

- Remeasure every callback expansion using the current full workload: both
  oscillators, modulation, locks, live edits, streaming and file operations.
- Run the full channel-budget one-hour zero-underrun soak, including stereo and
  Mono mixes. Preserve the earlier eight-MIDI-voice playback acceptance and
  panel-controlled filter/gain checks; both output/CV configurations must compile.
- Keep default-disabled DSP comparisons separate from production evidence.
  A voice-count change or backend port needs its own measured decision.

### 2.P — Panel controls and MIDI I/O (physical integration)

The keypad, temporary TLC5947 LED backend, MCP3208/RV112FF pot handling and
MIDI input/output firmware exist. Wiring and physical acceptance remain open.
Use [panel-controls.md](features/panel-controls.md) and the checklist:

- **HV-011:** TCA8418 wiring, matrix/key mapping, held-key recovery, shared
  touch behavior and latency.
- **HV-012:** TLC5947 electrical behavior, LED mapping and latency. Preserve
  the replaceable backend for the later PCA9956B.
- **HV-013:** RV112FF 20 kΩ waveforms, MCP3208 acquisition/settling,
  calibration, saved settings, feel and shared-bus timing.
- **HV-014:** DIN wiring and latency, USB enumeration, clock jitter,
  Start/Continue/Stop and SPP synchronization with a DAW.

**Gate (2.P):** from the panel alone, jump to Instrument, change cutoff with a
pot and hear it, latch Shift and fire a shifted softkey, then BACK out, with
correct LEDs throughout. DIN/USB MIDI notes sound; host and HIL tests pass.

**Phase gate:** program and perform a four-Track pattern with swing from the
panel; remain MIDI-clock-synced to a DAW for ten minutes without audible drift.

## Phase 2.5 — Sampler instrument layer

Instrument/Kit editors, two-oscillator voices, envelopes/LFOs, held-note
Apply/Revert and Mixer v1 are implemented. Continue from the
[Track/Instrument/Bank model](features/track-and-patch-model.md):

1. **Bank performance recall and editing follow-up.** The [Bank Manager and
   staged selected-Track recall](features/bank-persistence.md) are host-tested
   and compile-verified. New/Open/Save copy and explicit Store/Clear copy preserve
   immutable source files; physical SD/recall acceptance remains HV-016. Add
   preload and channel-routed MIDI Program Change recall after measuring service
   latency. Add slot-to-slot copy/move; decide whether a deferred unsaved Bank
   working copy improves the current explicit named-copy workflow. Persist the
   selected Bank path with Projects once its restore/admission policy is defined.
2. **Polyphony policy, after measurement.** Implement saved Mono/Auto/1–8-note
   caps and Own only / Own first / Any stealing with Track inheritance/overrides.
   Kit-wide and per-pad refinements remain proposed. Define stable note-group
   identity, transactional multi-layer/stereo admission, note-off/retrigger and
   old-file defaults. Include persistence, Apply/Revert, UI and audible-steal/DWT
   checks; this does not raise the global channel budget. See the
   [allocation proposal](features/project-menu-and-voice-model.md#instrument-and-kit-allocation-policy).
3. **Melodic sequencing.** Resolve the [note-length decision](#note-lengths-and-one-shot-playback--decision-pending),
   then add chord/tie lanes, step/live recording and erase.
4. **Remaining modulation.** Add MIDI CC/channel-pressure forwarding,
   additional destinations/UI and live lock recording. Analog/group lock
   lifetimes need a separate ownership design; voice-scoped lock application
   is already complete.
5. **Sampling/recording v1 and arpeggiator.** Rebuild recording against the
   voice/streaming architecture with fixed allocations outside the callback.
   Add admission-controlled concurrent streamed voices; the current SD/ring
   path is singleton-only. Preserve resident playback during these operations.
6. **Instrument browsing follow-up.** Add tag metadata/filtering.

**Gate:** from power-on, hear a card sample on the Keys in four taps; build and
save a 16-pad kit and a multisampled keyboard Instrument; load a Bank and recall
a slot with MIDI Program Change; record a chord progression and arp over a
p-locked drum pattern; complete a one-hour zero-underrun soak with both
oscillators active at `WAVEX_NUM_VOICES`.

## Phase 4 — Offline editing and mangling

Build a bounded Daisy-main-loop render-job scheduler, destructive editing,
crossfade-loop rendering, editor integration, slicing and offline mangling.
Use the central protocol reservations and CMSIS-DSP where appropriate.

Prepare reduced-rate assets for [vintage sampler grit](architecture-notes.md#vintage-sampler-grit):
27.7/22.05 kHz, companded 8-bit quantization, rate-based pitch, pre/post filters
and gentle saturation. The [playback model](features/vintage-sampler-math.md)
keeps the engine at 48 kHz and simulates per-voice sample clocks; complete
offline prints are optional. Runtime reconstruction remains unscheduled pending
a separate callback-capacity gate.

**Gate:** record, edit, slice, assign and sequence entirely on-device while
playback remains uninterrupted.

## Phase 5 — Performance and polish

- **Scenes/macros:** settle the proposed eight Project snapshots, explicit
  Store/Update, initial level/pan/mute contents, quantized manual recall and
  optional Song references. Resolve automation, stop/seek, deletion and Pattern
  authority before versioned storage and atomic recall.
- Send effects and their macro controls; tuning/scales.
- Instrument/Bank browser quality, Bank Select and “save with samples”.
- Device/Project settings ownership and persistence; USB sample import.
- Measured CPU/memory headroom pass. Consider polyphase sample-rate conversion
  only when profiling identifies a meaningful linear-interpolation cost or
  quality limit.

## Outstanding hardware verification

[hardware-validation.md](hardware-validation.md) owns procedures, blockers,
image identities and results for HV-001–017. Keep partial/deferred checks open;
formatting remains deferred until the user tests it. Save/load HIL and selected
bench passes do not replace physical or full-phase acceptance.

Additional physical checks retained from the earlier roadmap:

| Area | Remaining verification |
|---|---|
| Touch/display | Independent multi-touch release, Shift plus softkeys, two simultaneous dials/pads, navigation while held, five contacts, corner coordinates and wake from blanking; rotation/PPA corruption and tearing. |
| UI/input | Brightness, scrolling, MIDI filtering, encoder behavior, live Diagnostics and LVGL responsiveness during control bursts and sample loading. |
| Boot/settings | Partition migration and settings persistence through reboot. |
| Audio/storage | Correct 44.1/48 kHz pitch; UART traffic during streaming; SD read/write and hot-unmount soaks. |
| Filters | Audible SVF response, ladder self-oscillation and high-resonance/drive behavior using `scripts/bench_filter_listen.py`. |
| Sample retirement | Delayed/stopped callbacks and concurrent imports; timeout must preserve storage. Measure DWT headroom and soak after these transitions. |
| Sample Edit | Waveform fetch, handles, loop seams, browser detail and stereo readability beyond existing HIL coverage. |
| Load-to-Track/Pool | Hear Keys output and verify Pool-full/arena-full refusal. Paging, deduplication and replacement already have HIL coverage. |
| Region fades | Measure `Render()` with fades enabled before claiming a performance improvement. |

### Composition and performance workflows

The [Project/voice model](features/project-menu-and-voice-model.md) owns the
workflow design. Its implemented storage, stereo and Mixer work now has a single
validation home: HV-001–004 and HV-006–010; timing/soak remains HV-005. Assignment
and replacement must preserve selected-Track continuity and mix through held
notes, sequencing and reconnects. Future Scenes are scheduled only in Phase 5.

## Rules for every phase

- Apply the decision filter in [project-principles.md](project-principles.md).
- For protocol changes, update `protocol.h`, round-trip tests and
  [inter-mcu-protocol.md](features/inter-mcu-protocol.md) together.
- Follow [architecture.md](architecture.md) DMA/cache rules; callbacks never block.
- Run the [callback-headroom gate](performance_monitoring.md) at every phase
  gate and its event/calendar triggers: `< 70%` worst-case stays on H750;
  `70–<80%` blocks for investigation; `>= 80%` with callback features remaining
  activates backend upgrade planning.
- Phase gates include clean builds, host tests, bench HIL, reboot recovery and
  an appropriate zero-underrun soak. Host tests cover allocation/sample-load flow.
- Add/update physical checks in `hardware-validation.md` in the same change;
  close a roadmap gate only when its complete criteria pass.
- New subsystems need a focused design. Keep CMSIS-DSP aligned with libDaisy;
  update dependencies for an actual requirement or released upstream version.
- Keep one task here; remove completed work and delete superseded docs after
  preserving their open items. Git history is the archive.

## Next steps and backlog

These follow-ups retain their phase dependencies. Unscheduled ideas are not
accepted designs or permission to expand the current audio workload.

### Sampler and synth architecture references

Evaluate the [ASR-10/E4/EOS notes](architecture-notes.md) during related work:
visible edit scope, select-by-playing, resident variations, velocity articulation,
group editing, shared multisamples and bounded control processors. The
[Cord proposal](architecture-notes.md#eos-cords-further-modulation-lessons)
adds route-depth control, reusable mixes and explicit source lifetimes/evaluation.

Resolve the [workflow gaps](architecture-notes.md#workflow-coverage-and-remaining-design-gaps)
within their owning phases: sample-versus-Zone editing, reference-aware actions,
portable collections/missing-asset repair, bounded undo and record/arrangement
behavior. New voice features require ownership and callback-budget decisions.

### Oscillator drift — unscheduled

The [drift proposal](features/project-menu-and-voice-model.md#oscillator-drift-backlog)
adds bounded cents variation independently per voice/oscillator, through note-on
random offsets and/or slow drift. Default amount is zero; stereo pairs share
one pitch trajectory. Decide amount scope, global scale, rate/distribution,
retrigger and combined bounds before reusing/measuring the pitch-modulation path.

### Firmware audit remediation — 2026-09-06

Finish reference consistency checks against live transport/parameter behavior
across platform and feature guides. Preserve the distinction between host
coverage and hardware evidence (principles 13 and 15).

### Note lengths and one-shot playback — decision pending

Revisit the deferred [melodic sequencing draft](features/melodic-sequencing.md)
before implementing gates/ties. Decide sequence duration versus one-shot/gated
Instrument/Zone playback, release tails, overlapping notes, chord lanes, tempo
changes and transport-stop cleanup. Note-off scheduling belongs on the Daisy
audio clock with bounded storage and stable note identity. Fixed-length one-shot
regions used in benchmarks do not implement this behavior.

### Performance, build, and transport

- **UI responsiveness:** profile Settings, Project/Track, Pad Map, Pad Sound and
  Sample Edit when changing them, including encoder/touch bursts and loading.
  Existing measurements are in [UI latency notes](ui-latency-notes.md).
  Investigate expensive first frames in Sample Edit/Record/Play; profile smaller
  or filled waveforms before spreading construction across frames.
- **Compiler/placement experiments:** further optimizations remain deferred.
  Keep `-O2` as the accepted reference; global `-O3` was rejected. Measure LTO
  separately, including weak HAL symbols and placement. Benchmark remaining
  QSPI math helpers (`powf`, `tanf`, `arm_sin_f32`) before selective relocation.
  A persistent bootloader-SRAM alternative requires an image below 480 KiB,
  D1/D2/D3 headroom, matched streaming/voice soak and flash/boot/DWT evidence.
- **Event/link attribution:** distinguish trigger, lock and live-snapshot costs
  before moving preparation out of the callback. Investigate UART queue-full
  bursts and repeated polling; coalesce replaceable telemetry where justified.
  Rejected enqueue attempts are not automatically lost musical events.
- **Backend upgrade planning:** retain the [RT1170 plan](rt1170-migration.md),
  including the proposed M4 link/storage service after an M7-only audio baseline.
  Resolve shared-memory/cache handoffs, SD-stall command latency and bus contention.
  Planning does not authorize a port or hardware purchase.
- **Platform maintenance:** pin ESP-IDF to a 5.5 tag and run SD/panel checks;
  treat IDF 6 as a separate spike. Reduce overlapping ESP32 ownership as related
  modules change.
- **Build warnings:** bound the sample metadata page byte-count narrowing;
  resolve undefined `FF_USE_LFN` checks and the loader's enum/int conditional.

#### SPI-link revival requires hardware verification

UART remains production transport. The authorized [hybrid design](features/hybrid-inter-mcu-link.md)
keeps controls/confirmations on UART while evaluating SPI data/telemetry;
implementation and session recovery remain open.

Before adoption, investigate SD-write failures under combined SPI activity;
measure acknowledged-control latency, empty-frame/padding overhead and queue
backpressure. Scope receiver timing and repeat/soak the working mode/drive
combination before increasing the clock. Short passing trials do not establish
signal integrity or stability. Define CRC-rejection retry and stuck-READY/peer
reboot recovery; resolve exclusive SPI DMA ownership before Stage B CV sharing.
Use [SPI notes](spi-notes.md#verification-and-remaining-gates) for evidence and
[flashing](flashing.md#uartspi-comparison) for reversible comparison/rollback.

#### Streaming CRC recovery

Bound SD CRC recovery so the foreground cannot stall beyond the audio ring's
coverage: choose pause/recover, abort or additional prebuffer. Fault-inject the
choice and measure ring low-water and service latency.

### Memory and image size

- Move the loader's large `s_doc_storage` WXI working buffer into an explicitly
  owned SDRAM partition before adding another large resident buffer. Do not use
  `.sdram_bss` without reserving it in `sdram_layout.h`; it overlaps the sample
  arena under the current layout. Use current map files for headroom, not old
  SRAM percentages.
- Monitor the SRAM debug profile separately. If it overflows again, evaluate
  `-Og` or explicit D3 placement before assuming debugger-loaded ITCM/DTCM works.
- Consider gating USB CDC logging only after field logging through the UART
  bridge is confirmed. Removing unused USB host IRQ linkage requires a justified
  libDaisy submodule change.

### UI and frontend maintenance

- Reconcile Play's displayed octave and Pads/Keys range labels without changing
  MIDI note numbers.
- Break the UI/main dependency cycle after the UI hardware pass: inject a narrow
  shared context, move `ICommInterface` out of `main`, then remove the dependency.
- Give busy-overlay failures stable, non-spinning text and deliberate dismissal.
- Close the PCNT read/clear race with an IRAM-safe watch-point design or UI-task
  consumption.
- Recheck callers and remove unused frontend APIs, window manager/surfaces and
  the bypassed `mocks/ui_theme.h` in a separate cleanup.
- Supply root-menu resident-count/Instrument-name context from authoritative
  shared accessors rather than duplicating state.

### Samples, instruments, and browsing

- Reproduce non-frame-aligned WAV data artifacts with a minimal fixture; identify
  the parser/stream fault before changing offsets.
- Add bounded directory paging when the 500-entry browse target is required;
  current listings stop at 256.
- Define reference-aware rename/delete/reorder behavior and protocol. Sample
  unloading and resident metadata paging already exist.
- **Wavetable source, after 2.5:** follow [oscillator-sources.md](features/oscillator-sources.md).
  Resolve import metadata, cycle/frame limits, interpolation/anti-aliasing,
  modulation, saved chunks, RAM admission and DWT budget before scheduling.
- **Oscillator sync/FM, after 2.5:** decide master/slave direction, hard/soft
  sample sync, frequency/phase modulation, routing/depth/feedback, alias control,
  source compatibility, persistence and callback budget.
- **Waveform extensions:** reverse playback and Track/Zone-specific cursors
  follow future playback/context support; the current forward playback line is
  implemented and awaits HV-010.

### Hardware, diagnostics, and logging

- Fold legacy UART/Daisy logging into the module table incrementally; choose the
  release ceiling from field-diagnostics needs. Measure backend link counter
  cost before changing its release/build-profile policy.
- Evaluate a third filter topology only with a design and measured capacity:
  Korg35/diode-ladder candidates remain unscheduled. Share inactive topology
  storage if additional implementations justify it. The earlier ladder selection
  experiment is complete; the current ladder is ZDF.
- Fix wrapping resident Sample IDs in Diagnostics and verify a populated Pool.
  Audit active-voice/round-trip and MIDI/drop telemetry for remaining gaps before
  adding protocol fields. Recheck audition ACK routing so legitimate messages
  do not increment the Link tab's `unknown` count.
- Resolve dormant SPI pin reservation versus panel use only with the transport
  decision. Pin assignments and flags stay exclusively in the shared config.

## Phase 3 — Analog voice board (deferred)

Deferred until after the other phases; no analog hardware is currently planned.
Keep the Stage B design and both build configurations viable. If revived, choose
the CV DAC/voice count, prove PCM1690 TDM slot order, wire `TdmVoiceSink` and
`Mcp48Backend`, calibrate every voice and scope CV timing.

**Gate:** eight analog voices under sequencer control; calibration survives a
power cycle; CV updates arrive within one control tick.

## Related

- [Architecture](architecture.md) — system ownership and real-time constraints.
- [UI architecture](ui-architecture.md) — navigation and page lifecycle.
- [Hardware validation](hardware-validation.md) — bench procedures and results.
- [Changelog](../CHANGELOG.md) — completed work.
