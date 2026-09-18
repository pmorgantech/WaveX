# Hardware validation checklist

Use this checklist for periodic bench sessions: it records implemented behavior
that still needs physical validation, how to check it, and the result. The
[roadmap](roadmap.md) continues to own implementation order and phase acceptance;
this document owns the runnable checks and their validation status.

## Contents

- [Using and maintaining the checklist](#using-and-maintaining-the-checklist)
- [Prerequisites](#prerequisites)
- [Queue](#queue)
- [HV-001 — SD card formatting](#hv-001--sd-card-formatting)
- [HV-002 — Save free-space checks](#hv-002--save-free-space-checks)
- [HV-003 — Instrument save and recall admission](#hv-003--instrument-save-and-recall-admission)
- [HV-004 — Stereo and Mono physical checks](#hv-004--stereo-and-mono-physical-checks)
- [HV-005 — Phase 2 timing and soak gate](#hv-005--phase-2-timing-and-soak-gate)
- [HV-006 — Mixer controls and master](#hv-006--mixer-controls-and-master)
- [HV-007 — Project save, load and recovery](#hv-007--project-save-load-and-recovery)
- [HV-008 — Project Pattern management](#hv-008--project-pattern-management)
- [HV-009 — Song arrangement and playback](#hv-009--song-arrangement-and-playback)
- [HV-010 — Waveform playback head](#hv-010--waveform-playback-head)
- [HV-011 — Keypad interrupt and recovery](#hv-011--keypad-interrupt-and-recovery)
- [HV-012 — Panel LED output](#hv-012--panel-led-output)
- [HV-013 — MCP3208 and endless pots](#hv-013--mcp3208-and-endless-pots)
- [HV-014 — MIDI ports and clock serialization](#hv-014--midi-ports-and-clock-serialization)
- [HV-015 — LFO range and musical rate controls](#hv-015--lfo-range-and-musical-rate-controls)
- [HV-016 — Bank SD transactions](#hv-016--bank-sd-transactions)
- [Recording a validation session](#recording-a-validation-session)
- [Related](#related)

## Using and maintaining the checklist

When a change reaches a physical gate, add or update an entry **in that same
change**. Give it a stable `HV-NNN` ID, a roadmap/design link, the introducing
commit or feature, required equipment, steps and observable pass criteria.
Record prerequisites that prevent a run. Do not schedule unimplemented features
as ready tests or close a phase gate because its host tests pass.

Check a box only after the stated physical observation succeeds. Record the
date, tested image identities, setup and evidence below; partial runs leave
untested boxes open. Mark failures explicitly and link the fix/retest work.
Keep IDs and the latest result when a check passes; reopen affected checks when
a subsequent change invalidates that evidence. Link detailed logs/measurements
instead of copying them here. Update the matching roadmap gate when warranted.

The initial queue covers the current storage changes, stereo follow-up and
Phase 2 gate. Older outstanding areas remain discoverable in the
[roadmap hardware inventory](roadmap.md#outstanding-hardware-verification);
add concrete entries here as those areas next reach a bench gate.

## Prerequisites

- Both boards running the intended firmware; record each source commit, build
  profile and image identity. Follow [flashing.md](flashing.md).
- Serial logs/diagnostics and listening or measurement equipment appropriate to
  the entry. See [testing_guide.md](testing_guide.md#hardware-verification).
- For destructive storage checks, a **disposable or fully backed-up card**.
  Formatting erases all card data, including saved calibration. Identify the
  test card before confirming; defer that entry if it is not available.

## Queue

Pending means ready for a bench run; Deferred means intentionally postponed;
Partial means some checks passed; Blocked means a prerequisite is missing.
Use Passed or Failed after recording the corresponding evidence.

| ID | Check | Status | Latest result / prerequisite |
|---|---|---|---|
| HV-001 | SD format, confirmation and recovery | Deferred | User will test later; no physical format run |
| HV-002 | Full-card save rejection and recovery | Pending | Host/compile verified; near-full test card needed |
| HV-003 | Instrument save/recall admission | Pending | Host/compile verified; large and admitted WAV fixtures needed |
| HV-004 | Stereo/Mono physical follow-up | Partial | Switching reported working by user, 2026-09-16; remaining checks below |
| HV-005 | Phase 2 timing and soak | Blocked (full gate) | Timing/soak can be run separately; complete gate still needs roadmap prerequisites |
| HV-006 | Mixer controls, master and callback timing | Pending | Host/compile checks; physical audio and timing unrun |
| HV-007 | Project Save/Load/New and recovery | Partial | Automated round trip passed; reboot, failure injection and timing remain open |
| HV-008 | Project Pattern slots | Pending | Stopped workflow, panel, reboot and callback checks |
| HV-009 | Song arrangement and playback | Pending | Host tests; audio timing, panel, MIDI and DWT unrun |
| HV-010 | Waveform playback head | Pending | Host/render checks; tracking, UART, DWT and soak unrun |
| HV-011 | Keypad interrupt and recovery | Blocked | Firmware/host checks; physical matrix and INT wiring needed |
| HV-012 | Panel LED output | Blocked | Firmware/host checks; TLC5947 chain wiring and measurements needed |
| HV-013 | MCP3208 and endless pots | Blocked | Firmware/host checks; RV112FF waveform, wiring and measurements needed |
| HV-014 | MIDI ports and clock serialization | Blocked | Clock/SPP code and host checks; wiring, enumeration, latency and DAW timing open |
| HV-015 | LFO range and musical rate controls | Partial | WXI/Project settings and UI HIL passed; physical rates, reboot and timing remain open |
| HV-016 | Bank SD transactions | Blocked | Adapter host-tested; session/UI entry point required before bench execution |

## HV-001 — SD card formatting

**Introduced:** `954bbd7`. **Gate:** [Phase 2 card storage](roadmap.md#composition-and-performance-workflows).
**Setup:** both updated images, disposable card containing recognizable test
files, second card for replacement checks, serial logs and the physical panel.
**Behavior:** [card maintenance](features/inter-mcu-protocol.md#card-maintenance).

- [ ] **001a — Cancel:** open Settings → Storage → Format Card. Confirm the
  warning **ALL CARD DATA WILL BE LOST** and both Cancel / Erase all data are
  legible and usable. Cancel, then repeat and leave the page. Original files
  remain readable; neither action formats the card.
- [ ] **001b — Expiry/replacement:** wait over 60 seconds before confirming;
  separately replace the card during confirmation. Both stale confirmations
  must fail without erasing either card. A fresh confirmation is required.
- [ ] **001c — Format:** explicitly confirm on the test card while audition or
  sequencing is active. Playback stops safely; the page shows progress and
  eventually success. Old files are gone. Verify every directory in
  [card_layout.hpp](../firmware/daisy/src/storage/card_layout.hpp) exists.
- [ ] **001d — Resume/reboot:** copy a known WAV to the new card, browse and
  audition it, save an Instrument and Pattern, then reboot and reload both.
  Audio, link, UI and saves recover; no stale browser entries remain.
- [ ] **001e — No automatic erase:** boot with an unformatted/unsupported test
  card and try with no card inserted. A mount failure must not format anything;
  the page reports failure/not-ready rather than success.
- [ ] **001f — Interrupted result:** using only the disposable card, interrupt
  the link or restart the backend after confirmation. Reconnection must never
  replay erasure or claim success without a known result. Inspect/remount the
  card; if needed, recover through a fresh explicit format. An interrupted
  format is not expected to preserve card contents.

## HV-002 — Save free-space checks

**Introduced:** `954bbd7`. **Gate:** [Phase 2 card storage](roadmap.md#composition-and-performance-workflows).
**Setup:** backed-up near-full card with existing Instrument, Pattern and CV
calibration files; known playable samples; logs. Admission behavior is defined
in [architecture.md](architecture.md#card-saves-and-formatting-as-built).

- [ ] **002a — Instrument/Pattern:** attempt new-copy saves without enough
  space for the complete file plus directory headroom. Each reports insufficient
  space and creates no published file; existing files, playable Track and
  working Pattern remain intact.
- [ ] **002b — Calibration:** attempt calibration persistence on the full card.
  Logs report failure and the existing calibration file remains byte-identical.
  The runtime calibration response alone does not prove persistence.
- [ ] **002c — Query failure:** with the card absent/unreadable before saving,
  all three save paths fail rather than claiming success. Reinsert a readable
  card and confirm existing files still load. Do not pull the card mid-write
  for this check; that tests a different failure boundary.
- [ ] **002d — Recovery:** free enough space and repeat successful saves. Reboot
  and reload to verify persistence. Capture any delay from the first free-space
  query, UI/link recovery and resident-note stability; streamed audition stops
  for saves by design.

## HV-003 — Instrument save and recall admission

**Introduced:** `8fbb712`. **Gate:** [Instrument save admission](roadmap.md#composition-and-performance-workflows).
**Setup:** the bench's 49,250,304-byte direct-loaded PCM fixture, an admitted
smaller WAV, and sufficient card space so capacity does not mask admission.

- [ ] **003a — Rejected dependency:** direct-load the large fixture, make an
  Instrument edit, and try Save copy. It reports unsupported sample, creates
  no WXI, and preserves the playable Track and Revert point.
- [ ] **003b — Admitted dependency:** save an Instrument using the smaller WAV,
  reboot and reload. The Instrument sounds and its saved settings are restored.
- [ ] **003c — Both oscillators:** repeat rejection with the unsuitable sample
  only in oscillator 2. Its dependency must also prevent publishing a WXI.

## HV-004 — Stereo and Mono physical checks

**Gate:** [stereo/Mono follow-up](roadmap.md#composition-and-performance-workflows).
**Setup:** stereo fixture with clearly different left/right content, stereo
listening/recording path and physical panel. Existing automated evidence is in
the [stereo capture log](callback-performance-log.md#stereo-channel-verification--2026-09-16).

- [x] **004a — Switching:** user reported stereo/mono voice switching working
  on 2026-09-16. Exact image/setup and a recording were not supplied; this
  records that observation without closing the wider gate.
- [ ] **004b — Listen:** verify independent left/right content, Mono downmix,
  Project pan/balance and manual mute across Solo by ear. Digital meter checks
  alone do not establish the audible result.
- [ ] **004c — Physical controls:** exercise held-note/next-note Mono changes,
  Apply/Revert and WXI save/reload through the panel. Verify controls remain
  legible, responsive and consistent with the heard result.

Matched timing comparisons and the full-duration workload belong to HV-005;
the already recorded ten-minute captures do not satisfy the one-hour gate.

## HV-005 — Phase 2 timing and soak gate

**Gate:** [Phase 2](roadmap.md#phase-2--groovebox-core-sequencer-and-pads).
**Setup:** profiled images with recorded identities, current benchmark fixture,
logs and timing capture equipment. Use the existing
[stereo workload instructions](testing_guide.md#stereo-channels-and-project-mixing)
and [performance acceptance policy](performance_monitoring.md).

- [ ] **005a — Timing:** capture the four-Track sequencer's sample-offset timing
  and edit boundaries on hardware; compare measured results to the canonical
  sequencer gate and retain traces.
- [ ] **005b — Capacity/soak:** run the full channel-budget workload with
  streaming audition, modulation and Pattern save/load for one hour. Record
  worst-case callback headroom, underruns, queue drops and file errors. Require
  zero underruns and the performance guide's acceptance decision; retain the
  tested mix and duration rather than generalizing to untested configurations.
- [ ] **005c — Matched comparison:** capture the requested before/after stereo
  renderer comparison with equivalent workload/build conditions and image
  attribution in the callback performance log.

The complete Phase 2 gate also depends on unfinished MIDI clock, panel and
session-persistence work listed in the roadmap. Passing these checks alone
does not close it; add their runnable entries as those implementations arrive.

## HV-006 — Mixer controls and master

**Status:** Open — host/compile checks only. No physical results recorded.
**Gate:** [Mixer v1](features/output-routing-and-mixer.md),
[Phase 2 timing](roadmap.md). Introduced by the audible master/readback change.
**Setup:** Matching ESP32/Daisy images, stereo sustained sample, mono sample,
headphones and scope/audio capture; DWT and underrun telemetry available.

- [ ] Open Mixer from the menu and panel jump key. Check all eight strips plus
  master fit without overlap, fader and mute/solo targets work at panel edges,
  Tracks 9–16 page correctly, and encoder focus matches touch selection.
  Switching to/from Project and Sequencer preserves the selected Track and
  Solo. Manual mute remains set after Solo moves or clears.
- [ ] Confirm controls show external edits, are disabled after link loss/stale
  replies, and recover on reconnect. Reopen repeatedly during playback; record
  UI stack/heap headroom and redraw cost. Steady targets cause no continuous
  strip redraw.
- [ ] Play a short transient on Tracks 1 and 16 and a right-only stereo sample.
  Check the matching peak bars, gain/pan/mute effects and page switching.
  Master gain changes the master meters while Track activity remains pre-master.
  Open/close Mixer while counting 0x79 packets: approximately 25 Hz when open,
  zero after exit; unplug/reboot the frontend and verify the lease stops sends
  within three seconds. Stale UI bars clear rather than freezing.
- [ ] Repeat the DWT comparison below with meter capture both on and off.
  Record the added cost at the supported maximum mono/stereo voice workload.
- [ ] While voices and browser audition play, sweep master from silence to
  unity to +6 dB. Both sides follow; unity preserves the prior level and zero
  becomes silent after 5 ms. Meter levels follow the post-master output.
- [ ] Repeat fast reversals and identical repeated commands; scope confirms
  continuous ramps without resets or hard gain jumps. Verify legacy volume
  controls and mixer master read back the same accepted target.
- [ ] Compare DWT callback/control timing against the preceding image under
  the same maximum-voice/filter/modulation workload. Record average, p95 and
  maximum cycles, load, image identities and underrun counts; remain within
  the callback budget. Complete HV-005's separate soak gate.

**Blocker:** Hardware session pending. Record date, image hashes, measurements
and capture/log evidence here; do not infer physical success from host tests.

## HV-007 — Project save, load and recovery

**Introduced:** Project session transaction and Project files UI.
**Gate:** [Phase 2 persistence](roadmap.md#phase-2--groovebox-core-sequencer-and-pads),
[Project persistence](features/project-persistence.md).
**Setup:** Two boards, an admitted mono/stereo sample kit, a disposable or
backed-up card with spare capacity, serial logs and audio/DWT capture tools.
Use distinct Project names for each run; WAV dependencies stay at their card
paths. Preserve earlier files when testing failure/recovery.

- [ ] Build a session with multiple Tracks, edited filter/envelopes/Mono,
  MIDI routing, Track level/pan/manual mutes, master gain, tempo/input settings,
  groove, hidden steps and locks. Edit a referenced sample's trim/loop, gain,
  fades and channel mode. Save copy from Project > Project files.
  **Pass:** Resident playback continues, UI remains responsive, success occurs
  only after the new `.wxp` and all referenced WXI copies exist. Solo and live
  editor Revert state remain unchanged by Save.
- [ ] Cancel both Load and New confirmations, then confirm New and Load the
  saved name. Repeat after rebooting both boards.
  **Pass:** Cancel changes nothing; New starts empty/stopped; recall restores
  the saved audible settings, hidden steps and metadata. Manual mutes return,
  Solo clears and playback stays stopped until explicitly started.
- [ ] Keep an existing Project and attempt another copy under the same name;
  then use a nearly full card and an abandoned same-name asset directory.
  **Pass:** Clear failure, no overwritten files or current-session changes.
  Free space/use a new name and verify a successful retry.
- [ ] Remove a later Track's WXI/WAV dependency, change a WAV's dimensions,
  and repeat with too little free sample RAM to stage the whole session.
  **Pass:** Load fails before replacement, all old Tracks/PCM/edits/mutes remain
  usable, memory returns to the pre-job level and playback is left stopped.
- [ ] On the disposable card, interrupt power during Instrument-copy writing,
  Project writing and publication; reboot and load the previous Project.
  **Pass:** The earlier Project still loads. Incomplete new files never install
  partial state; orphan files are reported/preserved and are not overwritten.
  Record the interruption point and card/filesystem state for each case.
- [ ] Run a long load, leave/re-enter Project files and disconnect/reconnect
  the frontend link. **Pass:** Status polling never repeats a mutation,
  stale readback disables actions and a completed Load/New clears stale Solo
  and Sample editor metadata when returning to Mixer/Sequencer/Sample views.
- [ ] Measure DWT callback/control average, p95 and maximum cycles against the
  preceding image while saving resident playback and while recalling large
  Projects. Record foreground responsiveness during document construction,
  UART errors/queue pressure, peak RAM usage and underruns. **Pass:** No audio
  underruns and timing satisfies the performance policy; complete HV-005's
  separate soak gate. Do not infer timing from host test duration.

**Blocker/result:** Physical session not run. Record tested image identities,
date and evidence below; no reboot/durability/timing result is implied by the
host integration and widget tests.

### Save/load HIL — 2026-09-17

**Partial pass:** Four tests passed in 95.35 s on both boards (UTC transcript
2026-09-18 03:03:46). Project Save/New/Load restored Track MIDI/level/pan/mute,
master gain, tempo, swing, hidden step note/velocity/probability and LFO
0.01 Hz with 3/16 Sync. Cancelled Load/New, duplicate Save and missing-file
Load preserved the tested state. Standalone Pattern recall retained session
tempo and Track bindings; resident held playback continued while streaming
audition stopped. WXI recall preserved both LFOs, including 100 Hz, and the
Instrument UI preview/Apply/Revert/save workflow passed.

The initial run found an LFO-tab hang: Rate attempted to update a null unit
label. Updating the existing tile description fixed it; all four tests were
rerun together successfully. These are console/readback observations, not
analog-output, reboot, power-loss, near-full-card or callback timing results.
UART TX queue overflow messages occurred during the bench work; this run does
not establish a transport/soak pass. All broader checklist items remain open.

**Images:** Daisy persistent QSPI at `ef0995a2fde88d7eae3f92bd50c807cce9074e73`,
SHA-256 `6c9649364bd0a93e4fafb8f0b5a1acb4c2536b968232269563af64262013bad4`;
ESP32 from that revision plus the LFO tile-description fix,
SHA-256 `9c52b3d91de0af08ebd851b47cd6372ed5e482d22343407746ee657f2d934ab7`.
**Evidence:** local `logs/save-load-hil.xml`, `logs/save-load-hil-result.txt`
and `logs/hil-20260918-030346.log`. Reproduce with the
[save/load HIL command](testing_guide.md#project-pattern-and-instrument-saveload).

## HV-008 — Project Pattern management

**Status:** Pending hardware verification. Host/compile checks do not close this gate.
**Design:** [Pattern management](features/pattern-management.md), roadmap Phase 2.
**Introduced by:** stopped Project Pattern slot workflow.

**Setup:** Both updated images, resident Instruments on multiple Tracks, audio
monitoring, a writable card and DWT/render profiling per the performance guide.

- [ ] With sequencing stopped, put notes and locks on Track 16 / Step 64, shorten
  the visible Pattern, then copy active to slot 128. Create slot 2, select it,
  edit it and rename it. Switch among all three; every hidden step, lock and
  label returns, with no change to tempo, MIDI input, Instruments or mixer/Solo.
- [ ] Try Create/Copy on an occupied slot and Select on an empty one. Each fails
  without replacing any Pattern. Try Create/Copy/Rename while playing or
  MIDI-armed: Stop first appears and playback/arming continues until explicitly
  stopped. Launch behavior is covered below.
- [ ] Load a standalone Pattern file into a selected slot, switch away and back,
  then Project Save copy, reboot and Load. Active slot, other slots, labels and
  all step data return. Existing saved copies remain untouched.
- [ ] Disconnect/reconnect the frontend during Copy/Select and navigate away
  during an operation. Completion is recovered through reads, no operation is
  replayed, and no stale response enables a command on the wrong slot.
- [ ] Exercise touchscreen Previous/Next, encoder, naming and all softkeys;
  inspect both UI stack margins. Idle polling and identical replies do not
  cause repeated full-screen refreshes. Verify all controls remain legible.
- [ ] Record matched DWT average/p95/maximum and underruns before/after captures
  and selection with held/releasing voices. Check foreground responsiveness
  during first Project allocation and low-memory refusal. No deadline misses,
  no stopped-voice requirement for held live notes, and no sample corruption.

**Results:** Not run. Record date, both image identities and evidence here.
**Blocker:** Physical boards and measurement session required. Song playback
is not part of this entry yet.

Queued launch follow-up (host-verified, hardware pending):

- [ ] Use Patterns with different lengths/scales and distinguishable downbeats.
  Launch while playing at a tempo whose loop end is inside an audio block.
  Record/inspect the boundary: destination step zero starts on the outgoing
  full-loop grid, without an extra old downbeat or gap. Repeat under MIDI sync.
- [ ] Exercise negative offsets on both step-zero hits and retriggers on the
  outgoing final step. Old events do not cross the boundary; destination
  negative step zero clamps to launch. Held voice tails continue normally.
- [ ] Edit the outgoing Pattern while queued, including hidden locks; launch
  away and back and verify those edits. Send an old scoped edit after the
  switch: it must not modify the destination, even after returning to the same
  slot. Stop/restart cancels, reconnect never replays the launch.
- [ ] Sweep tempo mid-loop and while queued; timing remains continuous.
  Record matched DWT average/p95/max on launch blocks with full voice load,
  not just idle sequencing. Include loop-boundary launches in the soak.

## HV-009 — Song arrangement and playback

**Status:** Pending. No hardware execution recorded for this implementation.
**Design / gate:** [Song sequencing](features/song-sequencing.md),
[Phase 2](roadmap.md#phase-2--groovebox-core-sequencer-and-pads).
**Setup:** Both images from the same commit, two audible Pattern variations,
one short Pattern with a different length/scale, MIDI clock source, test card,
audio recording and DWT/underrun logging per the performance guide.

- **009a — Arrange and recall:** Open Sequencer → Shift → Patterns → Shift →
  Songs. Create/name a Song, change reference/repeats/tempo, insert, remove and
  move sections, including scrolling past section six. Apply and Revert must be
  distinct. Save a Project, reboot and Load; all sections, references, repeats,
  names and tempo must match. Empty references and deleting the last section
  must be refused. Pending operations must not repeat after reconnect.
- **009b — Boundaries:** Play a two-section arrangement at 123 BPM with repeat
  counts 2 and 3, then with unequal lengths/scales and negative micro-offsets /
  retriggers. Record audio: exactly the requested loops sound, the next section
  starts on its grid boundary, and no outgoing next-loop downbeat/retrigger
  crosses it. Voice tails must continue without unintended cuts.
- **009c — Stop, loop and seek:** Play once stops at the final boundary. Loop
  returns to section one. Start at a later selected section, Stop, select another
  and restart. The displayed playing section/repeat must follow audio, separately
  from selected edit row. Song Stop must reopen editing only after callback
  release; ordinary transport restart must exit Song mode into the current Pattern.
- **009d — Ownership and MIDI:** Try grid edits, Pattern launch and Project
  Save/Load during playback; frozen edits and competing storage jobs must be
  refused, while live notes/mixer remain usable. Repeat with MIDI arming,
  Start/Continue and Stop. No unrequested start may occur while waiting for clock.
- **009e — Capacity and rendering:** Use shortest Patterns at maximum tempo,
  dense retriggers/locks and the four-Track gate workload. Record DWT mean/peak,
  over-budget blocks, underruns and output continuity during transitions and
  start/stop. On the panel, compare idle polling, one field edit and section
  changes with RENDER/log-mode sysmon; unchanged polls must not redraw content.
  Run the separate full-duration HV-005 soak. Restore profiling defaults afterward.

**Pass:** Every case above passes with dated paired image identities and evidence.
**Blockers:** Physical boards, audio/MIDI capture and DWT measurements are required;
host and compile results do not close this gate.

## HV-010 — Waveform playback head

**Status:** Pending; no hardware results recorded.
**Design / gate:** [Waveform playback head](features/waveform-playback-head.md),
[Phase 2](roadmap.md#phase-2--groovebox-core-sequencer-and-pads).
**Setup:** Paired images from the same commit, mono and stereo WAVs at 44.1,
48 and 96 kHz, a long sample, overlapping playable notes, disposable test card,
UART diagnostics, audio recording and DWT/RENDER logging. Compare the previous
image under the same supported voice/streaming workload; record image identities.

- [ ] **010a — Position:** Load a sample so its waveform is available. Audition
  it in Browser, Sample Edit and Record preview. Check distinctive transients
  against audible playback, trimmed start/end and a long-file position. The
  line must track consumed audio, not jump ahead with SD reads. Both stereo
  lanes share one line. Unloaded files without waveform data have no line.
- [ ] **010b — Notes and loops:** Play pitched RAM notes and overlapping notes
  of the displayed sample (including a secondary oscillator). The newest
  matching note wins; when it ends, an older still-playing match may resume.
  Test retriggers, stealing and final Stop. Audition overrides matching notes;
  loop gaps hide the line. Check resampled streamed loops at all fixture rates.
- [ ] **010c — Editing:** Zoom/scroll during playback. Off-window positions
  disappear instead of sticking at an edge. Change trim/loop markers and check
  their handles remain distinguishable and touchable. Focus a loop splice:
  neither splice half gets a line; return to the continuous view to resume it.
- [ ] **010d — Lifetime:** Change sample, unload, leave/re-enter the page,
  stop/restart rapidly and interrupt/reconnect UART. No old sample's line may
  appear on the new view. On missing replies the line expires, without
  replaying an old request as an edit or blocking controls.
- [ ] **010e — Budget:** At the maximum supported voice workload with streamed
  audition, run matched DWT captures with the waveform open and closed. Record
  callback mean/p95/peak, over-budget blocks, underruns, UART queue/drops and
  command latency. Profile the panel with RENDER/log-mode sysmon: identical
  replies do not repaint; motion updates the old/new narrow strips. Complete
  the applicable HV-005 soak without underruns, then disable profiling.

**Pass:** All applicable checks pass with dated paired image identities,
recordings/logs and measured callback headroom. Visual tracking is display-rate
feedback, not a sample-accurate audio/visual synchronization promise.
**Blockers:** Physical boards and measurements unavailable in the host run.
Reverse playback and Track/Zone-specific views are not implemented; add their
checks when those features reach their own gate.

## HV-011 — Keypad interrupt and recovery

**Status:** Blocked on physical wiring; no bench results recorded.
**Design / gate:** [Panel controls, stage 2](features/panel-controls.md#keypad-int-implementation-stage-2-2026-09-17),
roadmap 2.P. Record paired firmware identities and wiring revision when running.
**Setup:** TCA8418 matrix and INT wired according to the canonical config headers,
shared touch bus, logic analyzer, serial diagnostics, and a playable test kit.

- [ ] Verify every mapped key and release in Diagnostics → Panel, including
  row/column, Shift, softkeys, transport and pads. Record actual geometry.
- [ ] Scope INT against FIFO service/UI dispatch; record latency under idle,
  busy UI and UART traffic. Roll ten transitions/chords; verify ordering,
  releases, no missing notes and no stuck pads. Compare with INT disabled.
- [ ] Disconnect INT only: safety polling still receives keys. Hold INT low:
  no task spin or touch starvation. Reconnect without false presses.
- [ ] Induce FIFO overflow and queue pressure. The diagnostic counters must
  reflect loss; accepted held keys release, and new normal input recovers.
- [ ] Disconnect the keypad I2C device during held input. Touch and audio must
  continue; no reset/abort, stuck notes or leaked bus ownership. Reconnect and
  verify recovery. Test orderly stop/start and missing-device boot as well.

**Pass:** Correct mapping and transitions, bounded recovery, responsive shared
touch, measured latency and zero audio underruns under the test workload.
Host tests cover register setup, FIFO ordering/ACK race, backpressure, overflow,
I2C failure and bounded draining; they do not establish physical correctness.

## HV-012 — Panel LED output

**Status:** Blocked on wiring; no hardware results recorded.
**Design / gate:** [Panel controls stage 3](features/panel-controls.md#led-output-implementation-stage-3-2026-09-17),
[roadmap 2.P](roadmap.md#2p--panel-controls-and-midi-io-physical-integration).
**Setup:** Temporary TLC5947 chain and LEDs wired from the canonical config
headers, appropriate current-setting resistors/supply and external BLANK pull-up,
logic analyzer/scope, paired firmware image identities, serial console, test kit.
Record wiring revision and images with each dated result.

- [ ] **012a — Startup and map:** Check power-on/reset/bootloader darkness before
  firmware starts, then Diagnostics → Panel → LED walk through every output.
  Record actual channel-to-LED mapping, including unused channels. Test all/off
  and ten-second expiry; leaving Panel restores normal policy immediately.
- [ ] **012b — Policy:** Verify root jumps, defined/disabled/active softkeys,
  sticky Shift, Play latch/held pads and release. On Sequencer check selected
  Track step bits, all four step windows, playing Pattern changes, Stop,
  record mode, offline link and page exit. `STATE`/`LEDS` must agree after service;
  build on existing `test_panel_keys.py` HIL navigation cases when hardware exists.
- [ ] **012c — Blanking and failure:** Let the screen sleep during normal and
  all-on output; LEDs must go dark and restore policy on wake. In a debugger,
  suspend only the UI task: outputs blank within the one-second heartbeat limit
  plus service latency, and recover on resume. Exercise orderly panel stop/start
  and injected SPI/init failure: blank, bounded retries, responsive encoders.
- [ ] **012d — Electrical/timing:** Scope complete-frame clocks, order and XLAT
  timing; no latch mid-transfer. Check brightness/flicker, rapid navigation,
  encoder turns, touch/key rolls and maximum UART traffic during playback.
  Record frame latency, panel-task stack headroom, encoder counts, UI rendering
  timings, UART drops and audio underruns. Compare matched pre/post workloads;
  no lost encoder movement, visible flicker or audio regressions are acceptable.
- [ ] **012e — Replacement seam:** Compile/select the PCA9956B stub; startup and
  encoders continue, diagnostics reports unavailable and no TLC control pins or LED frames are
  used. ADC scans may still own the shared SPI bus. Restore TLC selection for the bench image. Later PCA hardware needs
  a new validation entry; this check does not validate a PCA implementation.

**Pass:** All applicable checks pass with dated image identities and scope/log
or visual evidence. Host tests cover policy, mapping, PWM packing, blanking and
heartbeat wrap; builds do not establish electrical correctness. Repeat electrical
and flicker checks with MCP3208 scans active as specified in HV-013.

## HV-013 — MCP3208 and endless pots

**Status:** Blocked on wiring and waveform measurement; no hardware results recorded.
**Design / gate:** [Panel controls stage 4](features/panel-controls.md#pot-implementation-stage-4-2026-09-17),
[roadmap 2.P](roadmap.md#2p--panel-controls-and-midi-io-physical-integration).
**Setup:** MCP3208 and four Alpha RV112FF 20 kΩ pots wired according to the
canonical config headers and manufacturer terminal drawing; matched ADC supply
and reference, TLC5947 chain with external BLANK pull-up, scope/logic analyzer,
serial console and paired firmware. Record date, image identities, exact pot
suffix, wiring revision and scope/log evidence for every result.

- [ ] **013a — Electrical model:** Before calibration, measure each wiper through
  several slow revolutions. Record minima/maxima, relative phase and fold shape.
  Confirm quarter-turn triangular behavior; if it differs, stop and revise the
  decoder before enabling a pot. Compare raw `PANEL` readings to the scope.
  Measure settling, stationary noise and channel crosstalk with adjacent channels
  near opposite rails. The configured acquisition clock must settle adequately
  for stable controls; document effective resolution rather than assuming 12 bits.
- [ ] **013b — Calibration and persistence:** On a fresh NVS image all four pots
  must be disabled. Settings → Pots: select each pot, Start, turn twice, Verify,
  turn clockwise through a full revolution, Save. Clockwise must increase values
  regardless of wiper order. Reboot and verify ranges/direction/enabled state.
  Cancel during capture/verification must retain prior settings; incomplete,
  stuck or incompatible wipers must not pass verification. Disable must survive
  reboot. Inject an NVS write failure/full partition: show failure, retain active
  calibration, allow retry, and never erase other settings automatically.
- [ ] **013c — Feel and recovery:** Sweep all pots slowly and quickly across
  wraps, reverse direction and hold stationary. No stationary drift, large jumps
  or stuck motion. Disconnect/reconnect the ADC and each wiper using a safe test
  fixture; no spurious edits are acceptable (floating inputs may require hardware
  biasing). Verify the first sample after recovery/gap rebases without an edit.
- [ ] **013d — Bindings and rendering:** Play: Cutoff/Resonance/Attack/Decay;
  Instrument Filter: Cutoff/Resonance/Type/Model; Amp: Level/Pan. Check normal
  and Shift fine turns, value limits, tab/page changes and backend offline state.
  Confirm blank slots do nothing. Listen for the intended parameter changes.
  Inspect strips, touch targets and all resized page sections for overlap or
  clipping. Measure unchanged-state redraws and active-control frame times
  against the 30 FPS budget; console `POT` injection alone does not pass this gate.
- [ ] **013e — Shared bus and workload:** Repeat HV-012 frame/latch/flicker checks
  while scanning every ADC channel and turning pots. XLAT must follow only a
  complete LED frame. Exercise LED/ADC failure and recovery independently. Under
  maximum normal UART/playback traffic, key rolls and calibration saves, measure
  scan period, input latency, PCNT counts/overflow headroom, task stack, UI frame
  time and audio underruns against matched baseline workloads. No lost encoder
  movement, flicker, stalled UI or audio regression is acceptable.

**Pass:** All checks above pass with dated image identities and evidence. Host
coverage establishes synthetic decoder/calibration behavior and UI binding/redraw
logic only; the physical panel and Phase 2 gates remain open until measured.

## HV-014 — MIDI ports and clock serialization

**Status:** Blocked on DIN wiring confirmation and MIDI bench setup; no hardware
results recorded. USB can be tested independently.
**Design / gate:** [Panel controls stage 5](features/panel-controls.md#midi-port-implementation-stage-5-2026-09-17),
[roadmap 2.P](roadmap.md#2p--panel-controls-and-midi-io-physical-integration),
[MIDI sync](features/midi-sync-tempo-follower.md).
**Setup:** Paired firmware identities, current canonical config headers, correctly
wired isolated DIN receiver and buffered transmitter, USB host/DAW connected to
the board's HS OTG connector, MIDI monitor/analyzer and oscilloscope/audio capture.
Enable DIN only after confirming the receiver wiring; record image flags, wiring
revision, host and USB negotiated speed. Never infer port wiring from this doc.

- [ ] **014a — Enumeration and flags:** Confirm USB MIDI enumerates independently
  of the USB-Serial/JTAG flash port. Exercise input-only, output-only and disabled
  builds. Output-only must drain host OUT traffic without sounding notes; disabled
  output must reject submissions. Confirm no DIN GPIO activity with DIN disabled.
- [ ] **014b — Input notes:** Send notes/chords, velocity-zero NoteOn, explicit
  NoteOff and running status over DIN and USB. Verify selected Track MIDI input
  routing, including Off/Omni, sustained input, no spurious notes while idle and
  no regressions with both ports active. External clock ingest is exercised separately in 014f.
- [ ] **014c — Output bytes:** With a MIDI monitor/analyzer attached, send console
  commands such as `WAVEX-DBG 1 MIDIOUT USB START`, `WAVEX-DBG 2 MIDIOUT USB CLOCK`,
  `WAVEX-DBG 3 MIDIOUT USB SPP 16383`, `WAVEX-DBG 4 MIDIOUT USB CONTINUE`, and
  `WAVEX-DBG 5 MIDIOUT USB STOP`; repeat with DIN and BOTH. Check correct status,
  SPP low/high seven-bit bytes, USB cable/CIN and message order. Plain `MIDIOUT`
  reports per-port tuples ready,pending,accepted,sent,dropped,expired,failed.
  Accepted means queued; sent means accepted by the driver, not observed on wire.
- [ ] **014d — Congestion and lifecycle:** Saturate one output, stall host reads,
  suspend/unplug/reconnect USB and stop/restart port tasks while RX is active.
  No crash, stale callback, cross-port blockage or delayed tick burst is acceptable.
  Distinguish application backlog from packets already accepted by TinyUSB;
  measure any delayed driver-buffered output before accepting clock behavior.
  Verify queue-full/expired/failed counters and that Start/Stop replace queued
  backlog; an old queued Stop remains eligible. Observe that disconnect clears
  pending data. A failed USB packet write is reported and not retried. Confirm
  new submissions work after recovery; use fresh transport commands to restart.
- [ ] **014e — Latency and stability:** Measure DIN/USB note-input-to-audio latency
  (target under 5 ms) and output enqueue-to-wire delay/jitter, baseline and under
  display, SD/UART, panel-pot and LED load. Record median/p95/max, task stack and
  queue high-water observations, loss counters and Daisy underruns. No audio or
  UI regression; any clock jitter concern is measured before choosing a different
  hardware output owner. Console command round trips are not latency evidence.
- [ ] **014f — End-to-end clock (implemented, unrun):** In Sequencer select
  Internal; capture 24 PPQN at 20/120/300 BPM through both ports. Change tempo
  during playback and verify continuity. Stop must halt output. Select MIDI,
  press Arm, then send Start without Clock: no step may sound until the
  first Clock. Repeat with SPP 0, 8, 32 and 16383 followed by Continue. Verify
  the corresponding Pattern step for different lengths/scales; no earlier notes
  or retriggers may replay. SPP alone must stay silent. Repeat with a multi-section
  Song, seeking inside a repeated section and beyond the end, with looping both
  off and on. External Stop retains the paused Song; local Stop releases it and
  cancels the arm. Local Stop must work without any incoming clock.
  Supply simultaneous different DIN/USB tempos: only the first selected source
  may control the session. Re-arm to select the other source. Exercise duplicate,
  missing, grouped and out-of-order Clock delivery, a 300-ms dropout, tempo ramps,
  peer reboot and USB reconnect. MIDI-follow mode must not echo clocks.
  Run a ten-minute audio capture against the DAW metronome: drift within ±3 ms,
  no audible drift/breathing; record start offset separately. Repeat under
  display, panel and SD/save load. Port loopback/injected events cannot close
  this test or the full Phase 2 gate.
- [ ] **014g — Panel and callback checkpoint:** Confirm the Internal/MIDI and
  Stop buttons fit and respond after page exit/re-entry. Measured BPM and
  acquiring/locked/freewheel status must agree with the master. Idle polls and
  unchanged replies must not cause full-screen redraws; record RENDER/sysmon
  mean/peak and submitted pixels using the UI profiling guide. Compare DWT
  callback/control-tick mean/peak against the preceding image under the same
  stereo/mono voice, modulation, lock, SD and UI workload. Include maximum SPP
  seeks into a 128-section Song, busy MIDI input, dropped clocks, Start/Stop and
  full output queues. Record dated paired image hashes, callback cost and zero
  underruns using the performance guide's acceptance thresholds. Host tests
  and successful firmware builds do not establish real-time headroom.

**Pass:** Record date, both image identities and monitor/scope/audio evidence for
all applicable checks. Partial port validation remains partial; 014f and the
Phase 2 gate stay open until the required physical measurements pass.

**2026-09-17 software checkpoint:** Clock input/output, source selection, SPP
Pattern/Song seeking and UI controls implemented. Parser/queue/protocol and
transport/follower host tests added or updated. No board flashed and no new
hardware result recorded; all checks above remain open.

## HV-015 — LFO range and musical rate controls

**Status:** Partial: WXI/Project retention and LFO UI HIL passed on 2026-09-17;
see [save/load evidence](#saveload-hil--2026-09-17). Physical rate, reboot,
legacy-file and callback checks remain unrun.
**Design / gate:** [LFO controls](features/param-locks-and-modulation.md#lfo-rate-controls),
[Phase 2](roadmap.md#phase-2--groovebox-core-sequencer-and-pads).
**Setup:** Paired image hashes, a sustained sample with an Instrument LFO routed
at a modest depth to an audible destination, audio capture or scope, DAW MIDI
clock and a known older saved Instrument. Use the existing performance guide
for DWT/callback and UI RENDER/sysmon measurements.

- [ ] **015a — Hz range and adjustment:** With Sync Off, select 0.01, 0.1, 1,
  20 and 100 Hz. Measure periods (100 s, 10 s, 1 s, 50 ms, 10 ms) within 1%.
  Check slow-end fine adjustment, monotonic logarithmic movement, endpoint
  clamps and readable precision. Listen for unintended clicks/aliasing and
  characterize control-rate stepping at 100 Hz for each waveform/destination.
- [ ] **015b — Mode and tempo:** Set 0.1 Hz, switch Sync On, choose 1/4 then
  3/16. At 120 BPM measure cycles of 500 ms and 375 ms; at 60 BPM they double.
  Repeat while following external MIDI clock. Switching Sync Off restores
  0.1 Hz; held-note phase must not restart during these edits. Verify Gate/Free
  note admission behavior separately. Start/SPP do not reset held LFO phase.
- [ ] **015c — Persistence and recovery:** Apply/Revert and save/reload both LFOs
  with 0.01/100 Hz retained under Sync, including 3/16. Reboot and reload, then
  turn Sync Off and confirm the saved Hz rates. Recall the older Instrument:
  original division durations and Hz values must retain their identities.
- [ ] **015d — UI and callback:** Check value/unit changes, focus, fine controls,
  unchanged polling and page re-entry; record RENDER/sysmon submitted pixels
  and mean/peak. Compare DWT mean/peak to the prior image with identical
  stereo/mono voices, two LFOs per voice, modulation, locks, streaming and live
  edits. Exercise 3/16 at fast transport tempo and unsynced 100 Hz. Pass only
  within the performance guide's callback thresholds with zero underruns.

**Pass:** All applicable steps pass with date, both image identities, saved-file
identities and capture/profile evidence. Host tests cover numeric timing,
mode behavior and wire/WXI retention; they do not close these physical gates.

## HV-016 — Bank SD transactions

**Status:** Blocked on the working-copy session owner and device entry point;
this is a procedure for the next integration, not a runnable bench gate yet.
**Design / gate:** [Bank persistence](features/bank-persistence.md),
[Phase 2.5](roadmap.md#phase-25--sampler-instrument-layer).
**Setup:** Both integrated images, a backed-up disposable card, a sparse Bank
with occupied first/last slots and two-oscillator Instruments, serial capture
and DWT/audio monitoring. Use new names; preserve the source Bank.

- [ ] **016a — Sparse copies:** Create an empty Bank, store in slots 1/128,
  copy it while replacing one slot, then clear one slot into another copy.
  Reload each file. **Pass:** Stable slot identities, names/tags and both
  oscillator/LFO settings; the original Bank and its embedded bytes are unchanged.
- [ ] **016b — Failure isolation:** Try duplicate names, an occupied temporary
  name, insufficient free space and removed media. Cancel a long copy.
  **Pass:** No destination is published on failure, earlier files remain usable,
  and only the job's own temporary is eligible for cleanup. Failed reads never
  replace the live Bank or Track.
- [ ] **016c — Service budget and recovery:** Copy a full Bank while resident
  playback runs; measure foreground pump worst case (including one new WXI
  encode/read), callback DWT and underruns. Reboot and reload the successful copy.
  **Pass:** No underruns, acceptable control responsiveness under the performance
  policy, and the saved Bank reopens with the same slot contents. Record image
  hashes, card identity, timings and evidence before closing this entry.

## Recording a validation session

Append a record for each run and update the relevant boxes and queue status.
For a quick report, use IDs such as `001a passed; 001b failed — ...`.
Unrun subcases stay open. No physical tests were run when this checklist was
created; the checked switching observation above comes from the user.

```text
Date / tester:
Check IDs and per-case result (pass / fail / not run):
ESP32 image / source commit / build profile:
Daisy image / source commit / build profile:
Hardware, card, fixtures and relevant configuration:
Observed behavior / measurements:
Evidence (log, recording, trace or photo link):
Failure follow-up / remaining cases:
```

## Related

- [Roadmap and phase gates](roadmap.md)
- [Testing guide](testing_guide.md)
- [Flashing](flashing.md)
- [Performance monitoring](performance_monitoring.md)
- [Callback performance evidence](callback-performance-log.md)
