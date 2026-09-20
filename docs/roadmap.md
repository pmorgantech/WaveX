# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-20.

This document tracks remaining implementation, open decisions and phase gates.
Completed work belongs in [CHANGELOG.md](../CHANGELOG.md) and git history;
bench procedures and results belong in [hardware-validation.md](hardware-validation.md).
Code-complete features stay open there until their physical checks pass.

**Next software work:** Complete held-key ownership, saved polyphony policies
and controls, then the remaining Phase 2.5 tasks in order while panel wiring
is pending. Same-frame layered admission is implemented and its reproduced
16-Track deadline failure passes the extended pressure check. Residual capacity
and physical gates remain open below.

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
   Save As naming and sidecar versus render-to-new-file behavior.
2. Define partial-load behavior for samples too large for resident RAM.
3. Finish marker interaction: loop seam verification, zero-crossing snap
   protocol/policy for stereo, and playback-time loop crossfade.
4. Reconcile streaming/RAM channel behavior and expose `channel_mode` in the
   editor, including a label for one-channel views.

**Gate:** edit and audition a multi-minute WAV, save, reboot, reload, and hear
that region without a UI freeze.

## Phase 2 — Groovebox core: sequencer and pads

1. Verify sample-offset timing, edit boundaries and prepared note/velocity
   resolution for the four-Track gate (HV-005).
2. Close the [outstanding hardware checks](#outstanding-hardware-verification)
   for persistence, recovery, audio controls and feedback.
3. Complete panel integration and MIDI synchronization below.
4. Complete the callback capacity and one-hour soak requirements below.

Melodic notes and live recording follow in Phase 2.5.

### 2.C — Callback capacity checkpoint

Use [callback-performance-log.md](callback-performance-log.md) for measured
images and workloads. Same-frame admission now avoids work for layers stolen
before rendering. The 16-Track/four-layer pressure preset passes 605.86 seconds
and ten Pattern cycles at 66.0025% peak (66.8390% including setup), with zero
stream underruns. The earlier 74.5896% mixed-channel transition remains
unresolved; a ten-minute rapid mix rotation peaked at 59.9650% with zero stream
underruns and did not reproduce it. Repeat the complete
mixed-channel one-hour soak on the final image and retain HV-019 as partial.

**Continuation decision, 2026-09-20:** the user authorized proceeding after this
measured capacity pass while retaining unresolved findings here. Continue the
remaining sampler/sequencer work with per-change DWT checks; this exception does
not raise the thresholds, close a phase gate, authorize more render channels or
claim release readiness. Keep the backend-upgrade planning evidence available.

- Remeasure every callback expansion using the current full workload: both
  oscillators, modulation, locks, live edits, streaming and file operations.
- Repeat the full channel-budget one-hour soak after the admission change,
  including stereo and Mono mixes. Complete physical MIDI and panel-controlled
  filter/gain checks; both output/CV configurations must compile.
- Keep default-disabled DSP comparisons separate from production evidence.
  A voice-count change or backend port needs its own measured decision.

### 2.P — Panel controls and MIDI I/O (physical integration)

Complete wiring and physical acceptance using
[panel-controls.md](features/panel-controls.md) and the checklist:

- **HV-011:** TCA8418 wiring, matrix/key mapping, held-key recovery, shared
  touch behavior and latency.
- **HV-012:** TLC5947 electrical behavior, LED mapping and latency. Preserve
  the replaceable backend for the later PCA9956B.
- **HV-013:** RV112FF 20 kΩ waveforms, MCP3208 acquisition/settling,
  calibration, saved settings, feel and shared-bus timing.
- **HV-014:** DIN wiring and latency, USB enumeration, clock jitter,
  Start/Continue/Stop and SPP synchronization with a DAW.
- **HV-018:** 8-DSI-TOUCH-A display startup, touch coordinates, brightness,
  wake and rendering under load.

**Gate (2.P):** from the panel alone, jump to Instrument, change cutoff with a
pot and hear it, latch Shift and fire a shifted softkey, then BACK out, with
correct LEDs throughout. DIN/USB MIDI notes sound; host and HIL tests pass.

**Phase gate:** program and perform a four-Track pattern with swing from the
panel; remain MIDI-clock-synced to a DAW for ten minutes without audible drift.

## Phase 2.5 — Sampler instrument layer

Continue from the [Track/Instrument/Bank model](features/track-and-patch-model.md),
subject to the callback checkpoint:

1. **Polyphony policy.** Complete held-key/repeated-key
   ownership, Mono fallback and note-off/retrigger behavior on the whole-note
   runtime. Keep routed-MIDI admission/refusal observable: the combined
   16-Track/four-layer screen stayed below 70% but refused 540 complete notes
   at the bounded foreground queue. Do not equate console RX drops with musical
   admission failures. Add saved Mono/Auto/1–8-note
   caps and Own only / Own first / Any stealing with Track inheritance/overrides;
   define old-file defaults. Include wire/storage support, Apply/Revert, UI and
   audible-steal/DWT checks (HV-019). Kit-wide and per-pad refinements remain
   proposed; the global channel budget does not increase. See the
   [allocation proposal](features/project-menu-and-voice-model.md#instrument-and-kit-allocation-policy).
2. **Melodic sequencing.** Resolve the [note-length decision](#note-lengths-and-one-shot-playback--decision-pending),
   then add chord/tie lanes, step/live recording and erase.
3. **Remaining modulation.** Add MIDI CC/channel-pressure forwarding,
   additional destinations/UI and live lock recording. Analog/group lock
   lifetimes need a separate ownership design.
4. **Sampling/recording v1 and arpeggiator.** Rebuild recording against the
   voice/streaming architecture with fixed allocations outside the callback.
   Add admission-controlled concurrent streamed voices; the current SD/ring
   path is singleton-only. Preserve resident playback during these operations.
5. **Instrument browsing follow-up.** Add tag metadata/filtering.

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
image identities and results for HV-001–023. Keep partial/deferred checks open
until all acceptance criteria pass. Complete the following validation work:

| Area | Remaining verification |
|---|---|
| Card and save recovery | Wider-bus write reliability, fallback streaming performance, write soaks, near-full media and interrupted operations (HV-001–003). |
| Composition and mix | Project, Pattern and Song reboot/failure recovery, audio timing, Mixer/master, stereo/Mono, waveform tracking and LFO checks (HV-004, HV-006–010, HV-015). |
| Banks | Full-Bank recall/preload, MIDI Program Change timing, media failure/recovery and remaining Project/Bank restoration and rollback cases (HV-016). |
| Kits | Listening, reboot and failure recovery for complete kits and pad-addressed Patterns (HV-020). |
| Touch/display | Independent multi-touch release, Shift plus softkeys, two simultaneous dials/pads, navigation while held, five contacts, corner coordinates and wake from blanking; rotation/PPA corruption and tearing. |
| UI/input | Brightness, scrolling, MIDI filtering, encoder behavior, live Diagnostics and LVGL responsiveness during control bursts and sample loading. |
| Boot/settings | Partition migration and settings persistence through reboot. |
| Audio/storage | Correct 44.1/48 kHz pitch; UART traffic during streaming; SD read/write and hot-unmount soaks. |
| Filters | Audible SVF response, ladder self-oscillation and high-resonance/drive behavior using `scripts/bench_filter_listen.py`. |
| Sample retirement | Delayed/stopped callbacks and concurrent imports; timeout must preserve storage. Measure DWT headroom and soak after these transitions. |
| Sample Edit | Resident-sample selection (HV-017), waveform fetch, handles, loop seams, browser detail and stereo readability. |
| Load-to-Track/Pool | Hear Keys output and verify Pool-full/arena-full refusal. |
| Region fades | Measure `Render()` with fades enabled before claiming a performance improvement. |

### Composition and performance workflows

Validate assignment and replacement against the
[Project/voice model](features/project-menu-and-voice-model.md): preserve
selected-Track continuity and mix through held notes, sequencing and reconnects.
Use HV-001–010 for storage, audio and composition checks; Scenes remain Phase 5.

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
regions used in benchmarks are not acceptance evidence for gates/ties.

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
- **Exponential LUT candidate:** first benchmark the existing table-based libm
  `exp2f` against `powf(2, x)` for moving modulation and pitch locks. Consider a
  shared one-octave `2^x` table only if that leaves material cost. Keep these
  separate from code-placement trials. Use the existing CMSIS interpolation kernel,
  explicit startup initialization, finite/range handling and exact unity;
  measure numerical/pitch error and DWT cost before adoption. Sine LFOs, fades
  and integer note ratios are already tabulated. Filter tuning above 12 kHz
  and saturation tables need independent response/stability/listening checks.
- **Event/link attribution:** distinguish trigger, lock and live-snapshot costs
  before moving preparation out of the callback. Investigate UART queue-full
  bursts and repeated polling; coalesce replaceable telemetry where justified.
  Rejected enqueue attempts are not automatically lost musical events.
- **Browse request correlation:** the legacy directory reply has no request/path
  identity. Add a versioned correlated read before supporting safe cancellation
  of overlapping directory changes and automatic retry after arbitrary lost or
  delayed replies. Serializing bench requests is not proof of that behavior.
- **Backend upgrade planning:** retain the [RT1170 plan](rt1170-migration.md),
  including the proposed M4 link/storage service after an M7-only audio baseline.
  Resolve shared-memory/cache handoffs, SD-stall command latency and bus contention.
  Planning does not authorize a port or hardware purchase.
- **Platform maintenance:** pin ESP-IDF to a 5.5 tag and run SD/panel checks;
  treat IDF 6 as a separate spike. Reduce overlapping ESP32 ownership as related
  modules change.

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
choice and measure ring low-water and service latency. The absolute retry cursor
now preserves trimmed regions/loops; verify it under injected read failures at
HV-023 before claiming recovery acceptance.

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

#### 8-inch display bring-up

- [ ] Investigate font sizes and vertical layout on the 1280×800 panel.
  The display adds 80 pixels of height. Evaluate content space below the
  title/status bar, page tabs and soft-buttons on the real panel.
- Resolve tear-free landscape scanout and verify backlight registers on hardware
  (HV-018).
- If ten-contact support is adopted, add a GT9271-capable read path; the current
  GT911 driver rejects reports above five.

#### Other frontend maintenance

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
  extend the one-byte paging contract and bounded storage beyond 256 entries.
- Define reference-aware rename/delete/reorder behavior and protocol.
- **Wavetable source, after 2.5:** follow [oscillator-sources.md](features/oscillator-sources.md).
  Resolve import metadata, cycle/frame limits, interpolation/anti-aliasing,
  modulation, saved chunks, RAM admission and DWT budget before scheduling.
- **Oscillator sync/FM, after 2.5:** decide master/slave direction, hard/soft
  sample sync, frequency/phase modulation, routing/depth/feedback, alias control,
  source compatibility, persistence and callback budget.
- **Waveform extensions:** define playback/context support for reverse playback
  and Track/Zone-specific cursors.

### Hardware, diagnostics, and logging

- Fold legacy UART/Daisy logging into the module table incrementally; choose the
  release ceiling from field-diagnostics needs. Measure backend link counter
  cost before changing its release/build-profile policy.
- Evaluate a third filter topology only with a design and measured capacity:
  Korg35/diode-ladder candidates remain unscheduled. Share inactive topology
  storage if additional implementations justify it.
- Verify Diagnostics with a populated Pool and Sample IDs above 255; the wire
  entry, backend copy and table formatter already retain the 16-bit ID.
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
