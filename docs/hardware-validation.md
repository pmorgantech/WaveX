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
  strip redraw. Per-Track meters are still pending implementation.
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
