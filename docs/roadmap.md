# WaveX Implementation Roadmap

**Status:** Canonical implementation order. **Current phase:** Phase 2.
**Last updated:** 2026-09-22.

This document tracks remaining implementation, open decisions and phase gates.
Completed work belongs in [CHANGELOG.md](../CHANGELOG.md) and git history;
bench procedures and results belong in [hardware-validation.md](hardware-validation.md).
Code-complete features stay open there until their physical checks pass.

**Next work:** close recording/arpeggiator and global-LFO/held-lock physical
acceptance (HV-030–HV-032), resolve callback capacity and SD recovery, and finish
panel/MIDI integration. The user authorized the implemented continuation ahead
of capacity remediation; the above-threshold callback findings and all release gates remain open.

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

1. Complete the card-swap and socket power/signal comparisons in
   [HV-001](hardware-validation.md#hv-001--sd-card-formatting) to isolate the
   recorder single-sector CRC; retain driver timing as an open hypothesis, then
   resolve recovery after a 25 MHz write CRC and verify explicit retry.
   Complete standalone WXCF Save/Save As physical recovery, listening and
   loaded-audio checks ([HV-025](hardware-validation.md#hv-025--standalone-sample-saves)).
   Destructive rendering remains Phase 4.
2. Complete stereo snap/seam and playback-crossfade physical acceptance,
   including listening and full callback/soak checks
   ([HV-026](hardware-validation.md#hv-026--stereo-snap-seams-and-crossfade)).
3. Complete Recorded/Left/Right/Mono Sum physical acceptance: measure
   streaming/resident parity, listen and soak
   ([HV-027](hardware-validation.md#hv-027--sample-playback-channel-selection)).

File-backed editing was removed from scope by user decision on 2026-09-21.
[Resident admission remains all-or-nothing](features/offline-sample-editing.md#oversized-samples):
oversized files can stream for browser audition, but editing requires the
complete sample in RAM.

**Gate:** edit and audition a resident multi-minute WAV, save, reboot, reload, and hear
that region without a UI freeze.

## Phase 2 — Groovebox core: sequencer and pads

1. Verify sample-offset timing, edit boundaries and prepared note/velocity
   resolution for the four-Track gate (HV-005).
2. Close the [outstanding hardware checks](#outstanding-hardware-verification)
   for persistence, recovery, audio controls and feedback.
3. Complete panel integration and MIDI synchronization below.
4. Complete the callback capacity and one-hour soak requirements below.

### 2.C — Callback capacity checkpoint

**UPGRADE checkpoint, 2026-09-21:** eight Mono voices with extended modulation,
live locks and a Pattern file cycle reached **85.3713%**. The implemented
recording/arpeggiator continuation reached **86.5040%** in a short eight-Mono
screen with 20 seconds of internal capture (zero sampled stream underruns). Four-stereo short screens below 70% do not clear this result.
The user authorized sampling/recording and arpeggiator implementation ahead of
remediation on 2026-09-21. This continuation exception does not raise
[the recurring gate](performance_monitoring.md#callback-headroom-gate) thresholds
or close acceptance. Keep [backend migration planning](rt1170-migration.md) and
measured load remediation open.

Use [callback-performance-log.md](callback-performance-log.md) for measured
images and workloads. Investigate the unresolved 74.5896% mixed-channel
transition and repeat the complete mixed-channel one-hour soak on the final
image. Keep HV-019 partial; shorter passing runs do not close this gate.

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

1. **Polyphony validation (HV-019).** Complete audible-steal/retrigger,
   physical MIDI latency and extended final-image pressure checks. The short
   Mono-fallback screen reached 69.5417%, leaving little room below the
   continuation threshold. Per-pad policy refinements remain proposed; see the
   [allocation model](features/project-menu-and-voice-model.md#instrument-and-kit-allocation-policy).
2. **Melodic validation (HV-024).** Complete the
   [melodic hardware gate](hardware-validation.md#hv-024--melodic-sequencing),
   including physical MIDI, listening and the recorded progression soak.
   **Capacity deferred:** investigate the 77.1698% eight-note peak and earlier
   94.6304% 32-note overload peak. Both exceed the continuation threshold;
   zero sampled stream underruns does not close this callback gate. See the
   [DWT evidence](callback-performance-log.md#melodic-chord-pressure--2026-09-20).
3. **Remaining modulation.** Complete physical CC1/channel-pressure acceptance
   ([HV-028](hardware-validation.md#hv-028--midi-expression)) and expanded
   modulation/live-lock acceptance ([HV-029](hardware-validation.md#hv-029--expanded-modulation-and-live-locks)).
   Validate global LFO controls, held-step encoder locks, eviction notices and
   diagnostic counts (HV-032). Analog/group lock lifetimes need a separate ownership design.
4. **Recording and arpeggiator acceptance.** Complete codec-input and internal
   master-mix signal/listening, save/reboot/recovery and full-load recording
   checks (HV-030). Complete arpeggiator physical MIDI-clock alignment, generated
   note recording, persistence and pressure checks (HV-031). Add admission-controlled concurrent
   streamed voices; the current SD/ring path is singleton-only.
5. **Instrument browsing acceptance.** Validate tag editing, saved WXI filtering,
   pagination and storage-error recovery in [HV-033](hardware-validation.md#hv-033--instrument-tags-and-filtering).

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
image identities and results for HV-001–032. Keep partial/deferred checks open
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

### Frontend follow-up

Validate authoritative Diagnostics / MIDI counters and stale-state handling in
HV-032. Physical contrast, touch/encoder menu scrolling and rendering under load remain in
[HV-018](hardware-validation.md#hv-018--8-inch-display-bring-up).

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

### ESP32 frontend audit — 2026-09-22

The combined frontend reviews cover input, UART/router/listeners, response
caches, browser ownership, Play note/control lifetime and feature-page request
handling. The earlier metadata-pointer fix is retained in the changelog; copying
one entry does not resolve the remaining cross-task browser ownership issue.

The user authorized review findings 1–8 on 2026-09-22. Remaining implementation:

- **F3 — Browser ownership:** deliver complete bounded responses to the UI domain;
  stop UART callbacks mutating directory, selection, strings and metadata.
- **F4 — Sample identity:** separate pending load tags from confirmed resident
  selection; failure must preserve the previous editor sample.
- **F5 — Browse correlation:** see the single owning transport item below;
  delayed pages must never enter a new directory listing.
- **F6 — Play controls:** read authoritative per-Track values before relative
  input becomes an absolute CC; reject stale readback after Track/link changes.
- **F7 — Resident admission:** let the backend decide RAM admission, including
  reuse of an already resident file without another allocation.
- **F8 — Load binding:** retain rejected binding sends and confirm the resulting
  Track binding before claiming the sample is playable there.

Physical acceptance for all eight findings is [HV-034](hardware-validation.md#hv-034--frontend-note-browser-and-load-recovery).
Completed implementation leaves this list and is recorded in the changelog.
Keep the following earlier audit follow-ups independent:

- [ ] **UART link overflow counter has two writers and one lock.**
  `queue_overflows` is incremented from the link task's RX path without the TX
  mutex and from the send path while holding it, so the read-modify-write
  races across cores and the counter can under-report exactly when it matters.
  Diagnostics-only, but it is the pattern §14 asks us not to ship. Split the RX
  and TX overflow counters, or make them atomic.
- [ ] **`uart_link_stop()` leaves the mutex allocated when driver teardown
  fails.** It returns early on that path, so `s_uart_mutex` stays non-null and
  a later `uart_link_init()` takes its "already initialised" early return
  without reinstalling the driver — a silently dead link. Currently latent:
  nothing calls `uart_link_stop()`. Either finish the teardown on the error
  path or drop the unused stop entry point.
- [ ] **Dead packet-router singleton.** `packet_router.cpp` defines a
  file-scope `PacketRouter` and an accessor with no callers anywhere in the
  tree; the routing path uses the instance injected from `ApplicationContext`.
  It costs a static-init `std::function` construction for nothing. Remove with
  the unused-frontend-API sweep already listed under *UI and frontend
  maintenance*.
- [ ] **The link's router fallback fails silently.** When no router has been
  injected, the getter returns a function-local throwaway instance, so every
  inbound message would be parsed, counted and discarded with no diagnostic.
  Make the fallback say so once, or assert — an unreachable state should look
  unreachable.

The `ui_get_comm_interface()` global that two pages still reach through is
already covered by the `ICommInterface` item under *UI and frontend
maintenance*; the PCNT counter re-centre window is already covered by the
watch-point item in the same section. Neither is restated here.

### Melodic follow-ups

The [as-built melodic contract](features/melodic-sequencing.md) resolves gate,
one-shot, overlap, tempo and Stop ownership. Portamento/true legato, per-lane
probability/microtiming, scale-constrained pitch editing and external melodic
MIDI output remain deferred. Scale snapping specifically depends on the
unimplemented Phase 5 tuning/root/mask model; current note entry is chromatic.
Hardware acceptance stays in HV-024; do not treat
host tests or a short callback screen as the full Phase 2.5 gate.

### Performance, build, and transport

- **UI responsiveness:** profile Settings, Project/Track, Pad Map, Pad Sound and
  Sample Edit when changing them, including encoder/touch bursts and loading.
  Existing measurements are in [UI latency notes](ui-latency-notes.md).
  Measure entry, first use and re-entry of deferred controls in Sample Edit,
  Record and Play on device (HV-018e). Remeasure Locks mode against the historical
  243.52 ms baseline; the host invalidation regression does not establish device
  latency. Retain encoder/touch bursts, loaded rendering and physical acceptance.
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
  identity. F5 requires a versioned correlated read before supporting safe cancellation
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
choice and measure ring low-water and service latency. Verify the absolute retry cursor for trimmed regions/loops under injected
read failures at HV-023 before claiming recovery acceptance.

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
