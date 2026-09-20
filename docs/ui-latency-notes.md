# ESP32 UI responsiveness — 2026-09-13

This report records measured causes, fixes, hardware comparisons and remaining
limits for the Phase 2 ESP32 UI responsiveness work.

## Contents

- [Scope and outcome](#scope-and-outcome)
- [What caused the delay](#what-caused-the-delay)
- [Play and Instrument follow-up](#play-and-instrument-follow-up)
- [Matched hardware results](#matched-hardware-results)
- [Regression coverage and provenance](#regression-coverage-and-provenance)
- [Final hardware state](#final-hardware-state)
- [Related](#related)
- [Step Notes follow-up](#step-notes-follow-up--2026-09-20)

## Scope and outcome

Baseline: `40721eea3e3a2d1861429a78f37ce80aefdf6a22`, the committed UART
investigation, fast-forwarded to local `develop` and pushed to `origin`.
UI work is on `fix/esp32-sequencer-latency`. This is Phase 2 UI work;
Daisy audio scheduling, wire formats, transport configuration and sequenced
note-length semantics are unchanged in that September 13 comparison. Gates
were subsequently implemented; see the September 20 Step Notes results below.

The sequencer comparison candidate (`responsive2`) reduced average playback
press-to-confirmed-refresh time from **561.30 ms to 43.68 ms** (92.2%).
All 100 measured edits were confirmed by backend readback. Daisy reported
zero audio underruns and zero debug-command drops throughout the captures.
This is a software-path measurement on the physical boards, not an optical
measurement of the display or a new whole-Phase-2 acceptance gate.

## What caused the delay

- Grid cells acted on `LV_EVENT_CLICKED`, after release. The repeatable
  synthetic tap holds for about 105 ms, so that entire hold delayed the edit.
- Every 40 ms, the page reapplied unchanged row-button styles. Softkey refresh
  also rewrote six keys and the header Shift chip/rule. LVGL invalidates local
  styles even when their value has not changed.
- A pending edit temporarily made all 16 cells in its row unready. Rendering
  blanked/disabled the row and restyled it again when readback arrived.
  Default button shadows and state transitions amplified the work.
- Every baseline confirmed refresh submitted **921,600 pixels**, a full
  screen. LVGL's finite invalidation buffer falls back to a full-screen area
  on overflow. The observed many-widget invalidations match that mechanism;
  the experiment directly measures flushed pixels, not an overflow counter.

The UI service cannot consume an already-arrived reply while LVGL holds its
shared lock doing that work. The old 227 ms send-to-accepted interval therefore
must not be interpreted as UART wire RTT. The separate
[UART measurements](uart-baud-notes.md) measured sub-millisecond mean control
acknowledgements for the same normal UART configuration.

## Changes

1. Cache row selection, softkey appearance and Shift appearance. Always replace
   softkey callbacks and metadata, even when their visible labels match.
2. Build grid buttons with explicit flat styling, before setting geometry;
   omit inherited shadows and transitions.
3. Retain the last confirmed row while its readback is pending. Its snapshot
   does not authorize new toggles: the existing `Ready()` check still requires
   a validated reply. Link/window/global invalidation discards the snapshot.
   Existing bounded value-drag previews keep their original semantics.
4. Act once on `LV_EVENT_PRESSED`; holding and release do not toggle again.
5. Consume page updates every 16 ms. Background row requests remain spaced
   80 ms apart, with the existing 600 ms timeout. No new busy loop, task,
   cross-task callback or lock-order change was introduced.

## Matched measurements

Same physical boards, unchanged Daisy image/session and normal UART setting.
The workload was the eight one-shot drum Tracks plus four chord-note Tracks
from `scripts/bench_drum_chords.py`: 120 BPM, 32 steps, no sample loops or SD
preview, up to eight simultaneous engine voices. See the UART notes for the
sample-region and fixed-tempo chord limitations.

Each image had identical profiling and LVGL log-only sysmon options. Each
phase captured 20 seconds without edits, then 50 toggles of Track 1 / Step 2.
The even toggle count restores that cell's starting value. Baseline figures
below use `baseline-pixels`, which includes the same pixel instrumentation
as the candidates. Percentiles use the nearest-rank method.

| Image / phase | Press → confirmed refresh mean | p95 | Maximum |
|---|---:|---:|---:|
| Baseline, idle | 559.31 ms | 561.66 ms | 561.77 ms |
| Baseline, playback | 561.30 ms | 567.67 ms | 568.14 ms |
| Rendering cleanup only, idle | 332.44 ms | 333.69 ms | 334.63 ms |
| Rendering cleanup only, playback | 359.33 ms | 466.73 ms | 469.90 ms |
| Final candidate, idle | 43.47 ms | 49.86 ms | 49.88 ms |
| Final candidate, playback | 43.68 ms | 53.10 ms | 56.02 ms |

The rendering-only candidate still acted on release, blanked pending rows
and used the old service period. Its final refresh still covered the full
screen, so it was not the final fix.

| Final candidate, mean component | Idle | Playback |
|---|---:|---:|
| Press → edit submitted | 0.96 ms | 1.06 ms |
| Edit submitted → validated UI readback | 12.91 ms | 13.27 ms |
| Validated readback → refresh complete | 29.60 ms | 29.35 ms |
| Submitted pixels in that refresh | 44,943 | 49,065 |
| Maximum submitted pixels | 66,537 | 73,006 |

The final playback average refresh covers 5.3% of a screen. Baseline
release-to-refresh averaged 453.85 ms idle. In the final candidate refresh
completed before synthetic release in all samples, as expected for touch-down
handling; negative release-to-refresh values are valid.

| Steady LVGL busy time | Baseline mean / peak | Final mean / peak |
|---|---:|---:|
| Idle | 25.6% / 28% | 0.0% / 0% |
| Playback | 32.0% / 41% | 4.5% / 12% |

These are 300 ms LVGL sysmon reports (67–68 samples per window), **not ESP32
core utilization**. Zero means rounded below the monitor's resolution.
Both images continued around 28 reported refresh opportunities per second;
FPS alone did not reveal the input stall. See
[performance monitoring](performance_monitoring.md#part-2---esp32-ui-rendering)
for sysmon interpretation.

## Measurement boundary and reproduction

The opt-in UI profiling switch lives in
[`hardware_config.h`](../firmware/shared/config/hardware_config.h). It is off
in normal builds. `PAGE LATENCY` reports the latest cell interaction only:

| Field | Meaning |
|---|---|
| `id`, `row`, `col` | Trace sequence and zero-based visible cell |
| `down`, `up` | LVGL press/release event timestamps |
| `sent` | Successful existing step-operation submission |
| `read` | Matching request ID, validated page and expected step state accepted |
| `frame` | First LVGL `REFR_READY` after that acceptance |
| `pixels` | Areas submitted through `FLUSH_START` in that refresh |

Times are microseconds on the ESP32 monotonic clock. No log output occurs
inside the measured event/render path. The script queries the trace afterward.
Display callbacks are removed before the page's widgets are destroyed.

Use a separate ESP build/config as described in
[flashing](flashing.md) and [performance monitoring](performance_monitoring.md).
Enable UI profiling and optional log-only sysmon for that measurement image;
keep memory/performance overlays off. Flash ESP only to keep the Daisy session
identical, then run from the devcontainer with the HIL Python environment:

```bash
python scripts/bench_ui_latency.py --out logs/ui-latency/trial.json
python scripts/sysmon_stats.py --no-pages logs/ui-latency/trial-playback-steady.log
```

The script requires both boards and active serial loggers, uses the current
pattern, toggles one step an even number of times and leaves playback running.
On a failed run inspect the partial record and pattern before continuing.
Load the musical arrangement with `scripts/bench_drum_chords.py` only when
replacing the current session pattern and Track bindings is intended.

Synthetic touch exercises LVGL input/events, the UI lock, UART/backend readback
and real rendering/flush submission. It excludes touch-controller sensing and physical
finger timing. `REFR_READY` is not a panel scanout/photodiode timestamp, nor an
audio onset measurement. Physical touch and visual confirmation remain the
user-facing follow-up; do not claim every UI page now has these latencies.

## Validation and artifacts

- All pre-commit checks passed, including both normal firmware builds and
  the shared, ESP32 and Daisy host suites.
- Eight grid-model tests, including retained-snapshot authority and invalidation.
- Two real-LVGL softkey tests: unchanged appearance causes no redraw while the
  current callback still replaces the old one; empty/disabled/recreated keys
  preserve correct interaction and restore their border.
- 20 selected two-board HIL tests passed on the profiling candidate: navigation,
  touch/panel softkeys, Shift, step editing/page boundaries, press/hold/release
  and page exit while held, parameter locks, named pattern recall, and page
  teardown during waveform traffic.

Local ignored artifacts are under `logs/ui-latency-20260913/`: each candidate's
image, SHA-256 record, source patch, raw board/sysmon logs and JSON traces;
`comparison.json` holds aggregate figures. The accepted measurement image
`responsive2-esp32.bin` has SHA-256
`d2cbaf3a6919fba96a4823de4ff8076d7dce6212f1fcd69d7d3373618c19c978`.

A rejected intermediate candidate (`responsive`) passed a null transition
style descriptor and crashed on page entry. Pinned LVGL dereferences a found
transition descriptor during state updates. That call was removed; the final
candidate removes inherited styles before applying explicit styles instead.
The rejected image/ELF and crash logs are retained separately and contribute
no performance results. No vendored LVGL workaround was added.

## Sequencer normal-image checkpoint

The normal image was built with UI profiling and sysmon disabled. It is
`logs/ui-latency-20260913/normal-final-esp32.bin`, 1,259,024 bytes, SHA-256
`cf67fe23baa57fb05197cd2cab086531055f1c3885cf3c9217ae034eb989f564`.
Its ESP32-P4 image checksum/hash and fit in the existing application partition
were checked. After explicit user approval it was flashed successfully.
The normal-image smoke check confirmed profiling was off and a touch-down
edit/readback worked during musical playback, with zero Daisy underruns.
Further work now examines Play and Instrument redraws on the same branch.

## Play and Instrument follow-up

The user's follow-up expanded the investigation to Play Pads, Play Keys and
Instrument tabs. Static inspection found additional avoidable invalidation:

- Play's ordinary momentary press/release already uses the key's LVGL state.
  Latch/transpose refreshes instead rewrote labels and base colours across
  all 41 keys. The candidate tracks each key's applied base colour and note;
  it also refreshes the previously stale latch/octave/semitone tiles.
- Instrument sound/oscillator/modulator replies trigger parameter refreshes
  even when only request identity has changed. Shared value-tile and dial
  setters unconditionally rewrote font, text and focus colours. They now
  compare actual widget values before applying changes. No new parameter
  model or backend authority was introduced.
- Env/Filter curves reset their points on identical refreshes. The candidate
  compares the complete fixed-size point array and initializes each new line
  widget correctly, preserving page-owned pointer lifetime.

Three real-LVGL parameter-widget tests cover unchanged updates, actual value
changes, compact-to-numeric layout, focus/tone and copied handles. Before the
fix, 20 identical snapshots produced 40 flush callbacks; afterward they
produce zero. The existing compact-label path also retained its fixed width
when switching back to a number; the new layout regression test now passes.
These are host renderer results, not ESP32 latency or CPU measurements.
All formatting checks, both normal firmware builds and all three host suites
also passed with the broader candidate.

The broad measurement tool is `scripts/bench_ui_redraws.py`. It uses the
profiling-only `RENDER` command across Play's idle/momentary/latch cases and
all six Instrument tabs (idle plus alternating small edits). It stops the
sequencer and makes the selected Instrument's current sound its undo point;
subsequent benchmark parameter changes are reverted. Read its help before
using an arbitrary live session. The submitted-pixel threshold identifies a
full-screen-equivalent refresh; it does not independently count invalidation
buffer overflows. Average refresh cost is over refreshes submitting pixels.

The hardware comparison is complete. The baseline image
`pages-before-esp32.bin` includes the sequencer/chrome fix and counters, before
the shared-widget, Play and curve changes. Diagnostics was then measured on
that same baseline. The first combined fix (`pages-diag-after-esp32.bin`)
removed the full-screen latch and Daisy-tab repaints. Its remaining small
idle Instrument redraws came from the shared header resetting identical
text. The final candidate also guards title/context text and realigns only
when either changes.

Diagnostics compares displayed card text, warning colour and table cell text.
It keeps live sampling, time-series insertion, offline/warning presentation
and Freeze behavior. No telemetry cadence or audio processing was reduced.

### Matched hardware results

The accepted final comparison is `pages-before-r2.json` versus
`pages-final-r2.json`, and `diag-before.json` versus `diag-final.json`.
Each idle phase lasts five seconds after settling; interaction phases use ten
momentary/latch taps or ten alternating edits with confirmed state readback.
The sequencer is stopped, with the same twelve-sample drum/chord session and
Track 8 (zero-based) selected. The measurement images share the same profiling
and sysmon configuration. Screenshots are taken outside these windows.

Times below are **mean / peak milliseconds per pixel-submitting refresh**.
“None” means no refresh submitted pixels in that window, not a zero-time
render. Busy is **mean / peak LVGL sysmon percentage**, excluding the first
sysmon line of each phase because its interval can straddle the reset. It is
not ESP32 core utilization or end-to-end touch latency. Per-phase pixel totals,
frame counts and render/flush splits remain in `pages-comparison.json` and the
raw logs. These are short controlled comparisons, not a long soak.

#### Play

| Phase | Before refresh ms | After refresh ms | Before busy % | After busy % | Full-screen before / after |
|---|---:|---:|---:|---:|---:|
| play-pads-idle | 3.68 / 3.74 | None | 0.7 / 1 | 0.0 / 0 | 0 / 0 |
| play-pads-momentary | 2.68 / 3.73 | 1.99 / 2.30 | 2.2 / 3 | 0.7 / 2 | 0 / 0 |
| play-pads-latch | 33.81 / 105.34 | 2.54 / 14.84 | 19.1 / 24 | 1.6 / 6 | 10 / 0 |
| play-keys-idle | 3.69 / 3.73 | 2.85 / 4.15 | 0.8 / 1 | 1.6 / 3 | 0 / 0 |
| play-keys-momentary | 3.23 / 3.74 | 2.93 / 4.19 | 2.5 / 4 | 1.1 / 4 | 0 / 0 |
| play-keys-latch | 41.61 / 130.12 | 2.69 / 3.11 | 22.4 / 29 | 0.2 / 1 | 10 / 0 |

The ten latch taps previously caused ten full-screen refreshes on each
surface. Both final cases have zero. Pads latch peak area fell from 921,600
to 157,381 pixels; Keys from 921,600 to 40,919. The final Pads peak is higher
than the first combined candidate (14.84 versus 5.74 ms); both captures are
retained rather than selecting only the better peak. The final Keys idle
window still submitted small regions, so this is not a claim that every Play
idle interval becomes render-free. Ordinary momentary taps were already local.

#### Instrument

| Phase | Before refresh ms | After refresh ms | Before busy % | After busy % | Full-screen before / after |
|---|---:|---:|---:|---:|---:|
| instrument-Osc-idle | 26.72 / 26.80 | None | 11.2 / 15 | 0.0 / 0 | 0 / 0 |
| instrument-Osc-edits | 32.15 / 37.83 | 10.39 / 11.51 | 28.1 / 38 | 4.2 / 7 | 0 / 0 |
| instrument-Env-idle | 44.07 / 44.16 | None | 12.7 / 17 | 0.0 / 0 | 0 / 0 |
| instrument-Env-edits | 49.19 / 52.33 | 15.97 / 23.69 | 31.2 / 44 | 7.7 / 13 | 0 / 0 |
| instrument-Amp-idle | 23.88 / 24.08 | None | 9.0 / 11 | 0.0 / 0 | 0 / 0 |
| instrument-Amp-edits | 23.67 / 24.33 | 3.36 / 3.64 | 16.4 / 20 | 0.6 / 1 | 0 / 0 |
| instrument-Filter-idle | 28.00 / 28.13 | None | 10.9 / 13 | 0.0 / 0 | 0 / 0 |
| instrument-Filter-edits | 27.69 / 28.41 | 9.40 / 10.26 | 19.1 / 25 | 2.1 / 4 | 0 / 0 |
| instrument-Mod-idle | 28.57 / 28.68 | None | 11.8 / 14 | 0.0 / 0 | 0 / 0 |
| instrument-Mod-edits | 31.84 / 33.94 | 9.22 / 10.32 | 23.6 / 31 | 4.0 / 7 | 0 / 0 |
| instrument-LFO-idle | 38.95 / 39.03 | None | 13.1 / 18 | 0.0 / 0 | 0 / 0 |
| instrument-LFO-edits | 43.44 / 46.03 | 9.59 / 10.46 | 33.0 / 40 | 4.8 / 7 | 0 / 0 |

All six final Instrument idle windows submitted no pixels. Actual parameter
changes and their pending/confirmed status still render, and all sixty edits
completed with expected readback. Removing cheap unchanged-header frames can
raise the mean among the remaining refreshes relative to the intermediate
candidate; total work and busy percentage are the useful companion metrics.

#### Diagnostics

| Phase | Before refresh ms | After refresh ms | Before busy % | After busy % | Full-screen before / after |
|---|---:|---:|---:|---:|---:|
| diagnostics-ESP32 | 23.04 / 23.30 | 3.08 / 7.55 | 4.9 / 7 | 0.9 / 2 | 0 / 0 |
| diagnostics-Daisy | 106.30 / 106.42 | 2.73 / 3.37 | 24.7 / 39 | 0.5 / 2 | 10 / 0 |
| diagnostics-Audio | 17.74 / 17.99 | 1.30 / 2.45 | 3.9 / 5 | 0.5 / 2 | 0 / 0 |
| diagnostics-Link | 33.71 / 34.01 | 5.91 / 7.18 | 5.5 / 8 | 1.1 / 2 | 0 / 0 |
| diagnostics-Storage | 18.91 / 19.09 | None | 4.6 / 5 | 0.0 / 0 | 0 / 0 |
| diagnostics-MIDI | 8.24 / 8.32 | None | 2.0 / 3 | 0.0 / 0 | 0 / 0 |
| diagnostics-Panel | 14.31 / 14.44 | None | 3.3 / 5 | 0.0 / 0 | 0 / 0 |
| diagnostics-frozen | None | None | 0.0 / 0 | 0.0 / 0 | 0 / 0 |

Daisy-tab mean area fell from a full 921,600 pixels to 27,487, with a
33,097-pixel peak. Its refresh peak fell from 106.42 to 3.37 ms. Storage,
MIDI and Panel submit no pixels when their displayed data stays unchanged;
ESP32/Daisy/Audio/Link continue updating changing telemetry and history.
Freeze produced no redraws in both captures.

### Regression coverage and provenance

Ten two-board HIL tests passed on the first combined candidate (157.90 s):
Pads/Keys momentary and sustained latch notes, repeated toggle, transpose,
latch disable, tab switch and held exit; Instrument held-note preview,
Apply/Revert, filter modes, LFO/modulator/oscillator editing and saved recall;
sequencer edit boundaries and touch-down lifetime; and page teardown during
waveform traffic. The sustained Play fixture deliberately loops a short sample
so a lost note-off cannot pass because a one-shot simply ended. The normal
musical arrangement uses unlooped regions again afterward.

Seven further HIL checks passed on the final normal image (20.96 s): all
navigation cases and the sequencer touch-down/held-exit regression. An initial
normal-image invocation ran before the console was ready and skipped all
seven; those skips are not counted as passes.

The normal firmware builds and shared/ESP32/Daisy host suites passed. The final
header change also passed the ESP32 build/host suite, and formatting/lint
rechecks passed after their automatic corrections. The LVGL skill now includes
proactive repaint design and verification guidance and passes skill validation.
Shared widget changes cover other callers, but Settings, Track, Pad Map, Pad
Sound and Sample Edit do not acquire per-page timing evidence from these tests;
the remaining checks live in the consolidated roadmap.

Excluded trials are preserved: `pages-before.json` stopped because the harness
confused navigation `depth` with `moddepth`; that edit was reverted before the
corrected full baseline. `pages-final.json` left Play for Track during capture
for an unestablished reason. It is excluded, and the successful rerun checks
that the page stays unchanged through each capture. A screenshot attempt on
the profiling image had interleaved log text and failed its length check;
visual capture uses the normal image instead.

| Artifact | SHA-256 |
|---|---|
| Baseline `pages-before-esp32.bin` | `6f7e949da0261978815afc8b7e0c477a9ed9ed836c00191c197dd5dc28809f8e` |
| First combined fix `pages-diag-after-esp32.bin` | `0f277f33a56a8cf3cf77611a2443e6c9dd562dbc402b86d623e1ef319ff1b666` |
| Final profiling `pages-final-profile-esp32.bin` | `22bae0a9eb9c8a100e738f6cb890baa67b889f1594d26d796a817e513966e01f` |
| Final normal `pages-final-normal-esp32.bin` | `1b1c9505ac123ae1411cfd223c9238d8d3595937784ab9a6c0022149212dfc66` |

The final normal image is 1,260,816 bytes, has a valid ESP32 checksum/hash,
and fits the existing application partition. The final profiling image is
1,264,448 bytes. Prepared earlier `pages-after`/`pages-normal` images were
superseded by the Diagnostics/header-inclusive candidates. Exact captures,
image manifests and the complete source patch remain under the local ignored
`logs/ui-latency-20260913/` directory.

Reproduce each side with the appropriate profiling image already flashed:

```bash
python scripts/bench_ui_redraws.py --out logs/pages-candidate.json \
  --track 8 --count 10 --seconds 5
python scripts/bench_ui_redraws.py --diagnostics-only \
  --out logs/diag-candidate.json --track 8 --seconds 5
```

Do not use the physical controls during capture. A changed page invalidates
the run. Diagnostics mode measures seven tabs and Freeze; Play/Instrument mode
requires a loaded Track and temporarily edits/reverts its parameters.

## Final hardware state

The final normal image is flashed and running. Runtime checks reject both
profiling-only commands (`RENDER` and `PAGE LATENCY`), and normal sysmon is
disabled. Seven normal-image HIL checks passed. Eight integrity-checked
screenshots were inspected: Pads/Keys latched and released, Instrument
Env/Filter/LFO and Diagnostics Daisy. Curves, active focus, latch colours and
header alignment are present; the existing pitch-label and narrow sample-ID
column issues are recorded separately in the roadmap. Serial-log interference
occasionally invalidated a screenshot; only checksum-validated retries were
used for visual inspection.

The drum/chord arrangement is playing again at 120 BPM, 32 steps. The final
readback observed eight simultaneous voices, twelve resident samples, no SD
preview, zero Daisy underruns and zero debug drops. See
`current-hardware.json` and `measurement-restored-arrangement.json`.
Physical touch sensing, panel scanout/tearing, and the long audio soak retain
their existing hardware gates. This UI/planning work is included in the
`fix/esp32-sequencer-latency` release integration commit.

## Related

- [Roadmap and next steps](roadmap.md#next-steps-and-backlog)
- [UI architecture](ui-architecture.md)
- [Performance monitoring](performance_monitoring.md)
- [LVGL project skill](../skills/lvgl9/SKILL.md)

## Step Notes follow-up — 2026-09-20

The melodic editor preserves confirmed values while an edit is pending and
deduplicates repeated labels/focus styles. A corrected framebuffer capture,
`logs/melodic-notes-final.png`, verifies readable gate units and distinct
transport Play versus Mode: Play controls. It does not verify touch sensing
or physical scanout.

`logs/melodic-ui-sysmon2.json` and its per-phase logs record five seconds idle,
ten lane edits, ten selections and re-entry on the same profile image. SHA256:
`fd13850749735f798377cc7e33f3628b824981318030aa57964dd94c03c7bf15`.
Daisy used normal image
`2ec52b185dfa4bd956ec49212b8e6cfaa2ccf3d40f56236111ff7dff767194b6`.

| Phase | Submitted refreshes | Full-screen | Mean pixels | Mean / peak refresh |
|---|---:|---:|---:|---:|
| Idle | 0 | 0 | 0 | — |
| Lane edits | 19 | 0 | 271,489 | 53.40 / 54.51 ms |
| Lane selection | 9 | 0 | 18,825 | 3.26 / 3.45 ms |
| Re-entry | 3 | 1 | 492,049 | 94.51 / 186.61 ms |

Log-mode sysmon after the parser's default warm-up exclusion reports idle
0% LVGL busy time (15 samples); edits average 32.9%, peak 42% (8 samples).
These short screens characterize this page, not an A/B speedup or physical
touch latency. Local edits stay partial but exceed the 33 ms frame target;
pending/confirmed control restyling remains a possible refinement.

The first sysmon attempt stopped because Daisy already reported seven console
RX dropped bytes following flashing. The repeat explicitly recorded the
baseline and finish: seven in both, with zero stream underruns. It establishes
no additional drops during the UI measurement, not a zero-drop boot session.
An earlier counter-only image lacked sysmon; its results are retained separately
in `logs/melodic-ui-profile.json`. The final normal image disables profiling.

**Final half-step/feedback source:** `logs/melodic-ui-final.json` repeats idle
and ten edits, then replaces a lane through step recording and observes the
500 ms **Step updated** notice expire. ESP32 profile SHA256
`74ceed7265be9f51b5b70eae9cb1285a21ada9dc5d03f60106175acb3728432f`;
Daisy profile `a462346519a7783734f854f63e1dbc1b18ccdfa6445bf96a33f128e926a99730`.
Idle submits zero pixels. Edits submit 18 partial refreshes, mean 270,822 pixels
and 53.87 ms, peak 55.29 ms; feedback submits four partial refreshes, mean
111,483 pixels and 29.65 ms, peak 41.21 ms. Neither interaction submits a full
screen. Edit sysmon busy time averages 31%, peaks 43% (8 retained samples).
Both Daisy counters start and finish at zero. The short feedback trace checks
its rendering behavior, not a statistically stable performance distribution.
`logs/melodic-notes-halfstep.png` verifies the final half-step label/layout.
