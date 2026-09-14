# Daisy callback performance log

This is the durable report for the recurring callback-headroom gate defined in
[`performance_monitoring.md`](performance_monitoring.md#callback-headroom-gate).
One row represents one scenario, captured in its own serial log, on the
persistent QSPI `-O2` image with DWT profiling enabled. Host timings and the
`-O0` SRAM debug image do not belong here, and the helper refuses them.

Record a run with `make perf-record`. The helper reads the firmware's
`profile_config:` line (core clock, sample rate, block size, storage layout,
optimization level) and every `audio_callback` profiling window in the
capture, computes utilization from the raw `DWT->CYCCNT` counts against the
budget `core_hz * block_size / sample_rate`, and classifies the worst observed
callback. **Hz/block**, **Core**, **Image**, **Budget cycles** and
**Stream underruns** are read from the capture, not typed in. A trailing `+`
in **Commit** means the measurement came from a dirty tree.

**Stream underruns** counts the SD-stream ring running dry
(`AudioEngine::CheckAndLogUnderruns()` episodes), which is a refill problem and
can happen under a comfortable callback. It is not a callback overrun. Zero is
required for the gate to pass, but the cycle columns are what the **Decision**
band is made from.

The log deliberately records whether callback-resident features remain. At
80% or above that answer changes the architectural decision: remaining work
activates the backend chip-upgrade path; a feature-complete build is still
blocked from release or further callback scope until its margin is resolved.

## Filter topology A/B — 2026-09-14

Profiling QSPI `-O2` image at f9e823b, 480 MHz, 48 kHz, 48-sample blocks, driven
by `scripts/bench_filter_topology.py`: Tracks 0–7 bound to a full-file-looped
`/Drums/Kicks/bassdr01.wav`, every Instrument LP with the chosen topology,
`WAVEX-FILTER 24 100`, voice LFO 1 triangle at 0.05 Hz and Env 2/Env 3 at
6 s/6 s/0.4/6 s routed through four matrix slots into cutoff and resonance, one
held note per Track, cutoff alternated on all eight Tracks every 1.2 s, 200 s
per topology, no sequencer, SD stream or parameter locks. Zero underruns in both.

| Topology | Peak cycles | Peak | Active-window average |
|---|---|---|---|
| WaveX SVF | 145183 | 30.25% | 18.0% |
| DaisySP ladder | 335154 | 69.82% | 47.5% |

These are not gate rows (`callback_performance.py` requires 600 s), but the
delta is the number that matters: the ladder adds ~40 points for eight voices.
The modulation drives cutoff above Nyquist for part of each LFO cycle, where
both topologies take the exact-bypass path, which is why the averages swing
more than the peaks. Captures: `logs/perf-svf-mod-20260914-023223.log`,
`logs/perf-ladder-mod-20260914-022750.log`.

## Workload and evidence notes — 2026-09-07

Both captures used firmware commit 176ce1f, persistent QSPI -O2, 480 MHz,
48 kHz, and 48-sample blocks; only the filter topology/slope differed. Capture
metadata and image SHA256 values are in the local, gitignored logs/
perf-wavex-20260907-212459.json and logs/perf-daisysp-20260907-222524.json. The
WaveX run covered Track 0 pitches 60–67 at velocity 100,
/Drums/Kicks/bassdr01.wav (44.1 kHz mono, full-file loop), the singleton SD
stream, 120 BPM / 16 steps / 60% swing, eight simultaneous preview rows every
four steps, eight instrument modulation slots (LFO1, LFO2, and voiceLFO to
cutoff, gain, pitch, and pan; depth 8000; exponential and S curves), cutoff
alternating between 32768 and 60000 about once per second, resonance 45000,
and sustain 65535.

Parameter locks were programmed, but the current callback does not apply them;
they add no measured DSP workload and remain roadmap Phase 2 item 5. Both
captures recorded zero measured boot/runtime underruns. The measurements cover
the whole callback only; they do not separately time Render(), placement A/B,
physical DIN/USB latency, listening or loop-seam quality, hot-unmount recovery,
or the four-track sequencer gate. TSAN discovery was unavailable with
FATAL: ThreadSanitizer: unexpected memory mapping. Normal persistent firmware was
restored after profiling, and the final two-board console smoke run passed 7/7
selected tests (39 deselected).


## Track-addressed follow-up — 2026-09-07

Commit 66d0330 used the same persistent QSPI -O2, 480 MHz, 48 kHz and
48-sample-block configuration. The local capture is
logs/perf-wavex-20260908-003427.log (UTC filename); its companion JSON records
the clean commit, image SHA256, baseline and periodic device state.

This workload bound Tracks 0–7 to the same full-file-looped
/Drums/Kicks/bassdr01.wav and triggered MIDI note 60 on each. All eight
Instruments had eight modulation slots with the source, destination, depth
and curve settings described above. Each Track used resonance 45000 and
sustain 65535; cutoff alternated between 32768 and 60000 on all eight Tracks
roughly every 1.2 seconds. The WaveX 24 dB filter ran at 100% drive.
Sequencing used 120 BPM, 16 steps, 60% swing and simultaneous hits every
four steps. The other eight rows were muted, and the singleton SD stream
ran concurrently.

All sampled states retained eight voices and an active stream with zero
underruns or dropped commands. The DWT run remains in STAY. This is a
different workload from the earlier fixed-Track chromatic preview, so the
rows are capacity evidence rather than a direct performance comparison.
Parameter locks are still stored but not applied. Physical panel timing,
DIN/USB clock sync, audible loop quality and the full Phase 2 gate remain
unverified.

Normal persistent firmware was restored after capture. The final console and
Track-routing HIL selection passed 12/12 tests (39 deselected), including
scoped releases on four Tracks, rebinding, asynchronous SFZ replacement and
sample edits reaching future sequenced hits. Five new host voice-map tests
also passed, along with the required commit hooks.

## Touch kits and grid follow-up — 2026-09-07

Clean commit 271c8e3 used persistent QSPI -O2 firmware at 480 MHz, 48 kHz
and 48-sample blocks, with profiling enabled and DaisySP disabled. The
local capture is logs/perf-wavex-20260908-023420.log; its companion JSON
records the commit, image SHA256 and all sampled device states.

The workload created eight drum Instruments, one on each of Tracks 0–7,
with pad 1 assigned to /Drums/Kicks/bassdr01.wav and choke group 1. Choke
is scoped to its Track. Each Instrument used eight modulation slots with
the source, destination, depth and curve settings described above. Each
Track used resonance 45000 and sustain 65535; cutoff alternated between
32768 and 60000 on all eight Tracks roughly every 1.2 seconds. The WaveX
24 dB filter ran at 100% drive. At 120 BPM, 16 steps and 60% swing, all
eight rows triggered one-shot hits every second step; the other rows
were muted. The sample-pool record retained its full-file loop for the
concurrent singleton SD stream, while each drum zone used loop-off playback.
The touch Sequencer page remained open with live row readback and the
25 Hz playhead.

After warm-up, the runner observed 606.1 seconds; the capture contains 122
complete five-second DWT windows covering 610.2 seconds, including the
window already in progress when capture began. All 498 backend samples
retained eight voices and an active stream, with zero underruns or dropped
commands. All 50 frontend samples reported current readback, running
transport and zero dropped commands. Maximum callback usage was 67.0844%,
leaving 32.9156% headroom: STAY. This is capacity evidence for a different
workload from the previous looped-voice run, not a direct performance
improvement claim.

The host regressions cover kit ownership and failed saves, sparse and empty
WXI reloads, Track-local choke, wire round trips and stale grid replies.
These measurements do not verify physical panel wiring, MIDI clock sync,
audible quality or reboot/power-loss recovery. Arbitrary pad-note lanes,
velocity-aware prepared resolution, parameter-lock application and
pattern/song persistence remain open; the full Phase 2 gate is not passed.

Normal firmware was restored with profiling disabled and DaisySP disabled; the
final two-board HIL selection passed all 20 selected tests, including console,
kit save/reload, sequencer grid and Track-routing workflows.

The first full sixteen-pad persistence stress run at `e38df1d` did not
complete the ten-minute gate. `logs/perf-wavex-20260908-043405.log` and its
JSON metadata record three completed save/load cycles, then an I/O failure
during the fourth save at about 384 seconds. The stream reported FatFs
`FR_INVALID_OBJECT`, recovered by reopening, and accumulated 37 underruns.
The peak was 351404 cycles (73.2092%), also outside the headroom gate.
This short diagnostic run is not a passing capacity result. Follow-up work
limits cutoff preparation/publication to changed Tracks and stops the
streaming preview before accepted pattern Save/Load operations, matching
kit-save admission. Foreground failures now retain the first FatFs error
for diagnosis; the original invalid-handle cause is not established.

The first retest on `e5ed856` (`logs/perf-wavex-20260908-050136.log`)
failed its first save with `FR_DISK_ERR`; the short capture peaked at
321736 cycles (67.0283%) with no underruns. A diagnostic build then
reproduced the write/close failure at file offset 20672 with controller
error `0x00000006` (data CRC plus command-response timeout), captured in
`logs/perf-wavex-20260908-050641.log`. Both ran at 50 MHz. Pattern failures
now log that controller error before cleanup. The default clock was lowered
to 25 MHz for the next read/write validation; faster overrides require a
write soak as well as mount/read probes.

At 25 MHz, the original test directory still failed rename with
`FR_NO_FILE` (`logs/perf-wavex-20260908-051136.log` and
`logs/perf-wavex-20260908-051621.log`). A read-only trace showed only four
entries, including an interrupted temporary, while two previously completed
test saves were no longer visible. This is consistent with damage during
the earlier failed writes; it is not a filesystem-repair diagnosis.
A fresh, separate diagnostic directory passed six save/load cycles over
146 seconds with eight voices, zero underruns and zero dropped console
bytes (`logs/perf-wavex-20260908-051957.log`). That diagnostic image used
the same codec and file operations with only its directory redirected.
The production path remains `wavex/patterns`. At that point, no original directory had been
renamed or deleted; preserving/recreating it awaited user authorization.
The temporary listing and rename instrumentation was removed. A retained
visible-file restart check passed after backend restart (`logs/pattern-reboot-
check.json`), preserving note, groove, tempo and Track bindings. The full
persistence soak and new-save recovery were still blocked by the original
directory failure; the short fresh-directory run did not satisfy the
ten-minute gate.

## Full-kit audio/grid follow-up — 2026-09-08

The clean `70deebd` run completed 605.2 seconds (121 profiling windows) at
68.3521% maximum utilization, 31.6479% headroom and zero underruns: STAY.
Its 328090-cycle maximum compares with 337649 cycles (70.3435%) on
`38b83b0` using the same eight-Track, fully populated sixteen-pad kit
workload and SD clock. The change combines cutoff and resonance into one
WaveX coefficient calculation per retune; host tests verify equal output
through live tuning, bypass, slope and sample-rate transitions. Average
utilization was 25.8%, up from 24.8% in the preceding run.

Both runs used the WaveX 24 dB filter at full drive, eight modulation slots
per Track, simultaneous hits every second step, changing step notes, live
cutoff edits, a looping SD preview and the touch grid's readback/playhead.
The latter run recorded 50 UI samples and retained eight voices throughout,
with zero sequencer queue-drop messages and dropped console bytes.
`logs/perf-wavex-20260908-060531.json` records the clean commit, binary
SHA256, confirmed SD mount clock and device states.

Normal firmware was restored with profiling and DaisySP disabled. The final
console smoke passed all eight selected tests in 2.01 seconds. A subsequent
retained-file restart check on that restored image loaded `Perf 043405 75`
with note 75, step bits 21845, swing 60, tempo 149, empty Track bindings,
zero samples, zero voices and zero underruns; the check ended back at the
Sequencer home state with playback stopped and tempo 120.

File operations were excluded because the original pattern directory
was unresolved; this audio/grid capacity pass does not satisfy the
persistence soak or the overall Phase 2 gate. The earlier retained-file
restart check (`logs/pattern-reboot-check.json`) successfully loaded
`Perf 043405 75` with its notes, step bits and swing intact while preserving
session tempo and empty Track bindings. No original directory had been renamed
or deleted at that point.

## Pattern directory recovery and persistence follow-up — 2026-09-08

With explicit user authorization, the idle backend renamed `wavex/patterns`
to `wavex/patterns-failed-0908` and created a fresh `wavex/patterns`. The
maintenance operation first checked that the source was a directory and the
archive path did not exist. FatFs returned success for both operations and
subsequent directory checks, captured in
`logs/pattern-directory-recovery-0908.log`. The original directory was
preserved without deleting its contents; this is not a claim that its
damaged or missing entries were repaired. All temporary maintenance code
was removed before rebuilding and flashing the measured image.

Clean commit `27b7fd6` then passed the full persistence workload on the
production path at the default 25 MHz SD clock. The runner observed 606.0
seconds; 121 complete DWT windows cover 605.2 seconds. Maximum callback
time was 330933 cycles (68.9444%), leaving 31.0556% headroom: STAY.
Average utilization was 25.8%. All 473 periodic backend samples retained
eight voices and an active stream, with zero underruns or dropped console
bytes. The capture contains no sequencer queue-drop messages or pattern
I/O errors; 48 UI samples confirmed live grid readback and playback.

The workload used the eight fully populated sixteen-pad kits, Track-local
choke, 64 modulation slots, WaveX 24 dB filter at full drive, live cutoff
edits and alternating-step note lanes described above. Six new-copy saves
and confirmed loads completed at approximately 97, 193, 289, 385, 482 and
578 seconds. File operations stopped the streaming preview while resident
voices continued; the driver restarted the preview and sequencer after
each load. Parameter locks remain stored but unapplied.

`logs/perf-wavex-20260908-102335.log` contains the capture; its companion
JSON records the clean commit, image SHA256, SD mount clock, file results
and device states. This passes the ten-minute persistence workload and
callback capacity check; it does not verify arbitrary power interruption,
physical panel operation, MIDI-clock synchronization or the full Phase 2 gate.

Normal firmware was rebuilt and flashed to both boards with Daisy profiling
and the experimental DaisySP filter disabled. After backend restart,
`Perf 102335 450` loaded successfully with note 75, step bits 21845 and
swing 60 while preserving session tempo 149 and all empty Track bindings.
The check began with no resident samples or voices and ended at Home with
the sequencer stopped and tempo 120; Home here means the Main Menu action.
The retained result is
`logs/pattern-reboot-check.json`. The subsequent console and pattern-file
HIL selection passed all nine tests (45 deselected, 36.11 seconds),
including hidden-step values, mute, duplicate-name rejection, confirmation
cancellation, missing-file handling and resident voice/Track preservation.

## Pad sound editing and combined tuning — 2026-09-10

The per-pad sound workload adds filter/envelope overrides on pads 1, 5, 9
and 13 of all eight kits, then alternates Pad 13 cutoff between 1200 and
18000 Hz while the existing live Instrument cutoff edits continue. The
remaining workload matches the preceding persistence run: sixteen populated
pads per kit, Track-local choke, 64 modulation slots, WaveX 24 dB at full
drive, 25 MHz SD streaming, step-note playback, touch grid readback and six
pattern save/load cycles. Parameter locks remain stored but unapplied.

The clean pad-editor commit `3e415cf` completed all file operations with
zero underruns, console drops or sequencer queue-drop messages, but its
maximum callback reached 339751 cycles (70.7815%, REVIEW). The baseline is
`logs/perf-wavex-20260911-011618.log` and its companion JSON.

A follow-up on clean `f904032` submits cutoff and resonance together at
voice trigger and inherited live updates, eliminating the intermediate
coefficient recalculation. Pad-owned cutoff remains unchanged by Instrument
edits; filter integrators, modulation and envelope ownership are preserved.
The focused filter/voice selection passed 84 host tests, including equivalent
output across topology, slope, drive, sample rate, clamping, bypass, reset
and topology-switch transitions. The commit's Daisy build and full host
suite also passed.

The same bench driver then observed 605.7 seconds; 121 complete DWT windows
cover 605.2 seconds. The maximum was 333170 cycles (69.4104%, STAY), leaving
30.5896% headroom. This is 6581 fewer peak cycles than the baseline in these
two runs; it is not a bound on every future workload. Average utilization
was 25.7%. All 465 periodic backend samples retained eight voices and an
active stream, with zero underruns or dropped console bytes. Six save/load
cycles and 47 live grid samples passed; no file errors or sequencer
queue-drop messages appeared. Streaming preview stopped for each file
operation and restarted afterward while resident voices continued.

The capture is `logs/perf-wavex-20260911-013012.log`; its companion JSON
records the clean commit, image SHA256, SD clock, file results and states.
This passes the measured workload and capacity checkpoint. Physical panel
operation, arbitrary power interruption, MIDI timing and the full Phase 2
gate remain unverified.

Normal firmware was rebuilt and flashed to both MCUs with profiling and
the experimental DaisySP filter disabled. After backend restart,
`Perf 013012 450` loaded with note 75, step bits 21845 and swing 60,
preserving session tempo 149 and all empty Track bindings. The saved
`HIL kit 1789088901` then loaded into Track 3 with Pad 16's override
enabled, cutoff 1200 Hz, amp attack 7 ms, decay 250 ms and sustain 35%.
Both checks started with no resident samples; their results are retained in
`logs/pattern-reboot-check.json` and `logs/pad-sound-reboot-check.json`.
These verify completed saves across restart, not interruption during a write.

The final normal-firmware console, kit-editor and pattern-file HIL selection
passed all ten tests (44 deselected, 62.09 seconds). This includes the default
build rejecting the experimental filter, sparse kit sound save/reload and
inheritance, hidden pattern steps, duplicate/missing files and preservation
of another Track's voice. The devices finished at the Main Menu with no
resident samples, voices or stream and zero underruns; the retained state is
`logs/pad-sound-final-state.json`.

## Two-source renderer placement diagnostics (2026-09-12)

These four short, dirty-tree captures diagnose cost; none is a phase gate.
They use eight Tracks with two independently resolved 16-pad maps each,
different resident kick samples, a 50/50 mono submix and Oscillator 2 detuned
17 cents. The workload retains eight modulation slots per Instrument, four
applied locks per hit, WaveX 24 dB full drive, live Track/pad cutoff edits,
25 MHz SD streaming, the touch grid and periodic pattern save/load.
All runs completed one file cycle. Image hashes and setup/readback evidence
are retained in each capture's adjacent JSON.

| Change | Duration | Mean cycles | Peak cycles (budget %) | Underruns | Capture |
|---|---:|---:|---:|---:|---|
| Initial paired-source renderer | 126.1s | 194248 | 466089 (97.1019%) | 0 | `perf-dual-wavex-20260912-013126.log` |
| Inline source reader; split profiling zones | 126.2s | 172181 | 406031 (84.5898%) | 0 | `perf-dual-wavex-20260912-013854.log` |
| Renderer in ITCM | 126.1s | 154868 | 401614 (83.6696%) | 0 | `perf-dual-wavex-20260912-014331.log` |
| Renderer and event paths in ITCM | 125.1s | 151645 | 304316 (63.3992%) | 0 | `perf-dual-wavex-20260912-014917.log` |

The source reader initially remained out of line in the per-sample loop.
Inlining removes that call and its frame/end-state spills. Separate
`voice_events`, `voice_modulation` and `voice_render` profiling zones then
identified the renderer's steady cost and event-path spikes. Selective ITCM
placement covers Render, Trigger, source initialization, live-parameter
application, prepared-map resolution, lock application and sequencer dispatch.
Named subsections keep ordinary functions separate from header COMDAT groups;
the existing startup copier installs them before audio starts. No voice count,
filter/drive setting, sample rate, block size or DSP algorithm was reduced.
The short runs justify a clean-commit ten-minute gate; they do not replace it.

The 2026-09-12 placement diagnostic moved `VoiceLfo::Start` and
`VoiceManager::TickModulation` into ITCM after the clean LFO run identified
them in QSPI. The dirty build-profile-edit image measured 125.035 seconds
over 25 windows, 152,798 average cycles and 313,693 peak cycles (65.3527%),
with zero underruns/drops and one file cycle. Capture metadata is in
`logs/perf-lfo-wavex-20260912-041514.json`. This is placement-only diagnostic
evidence, not an accepted clean-commit gate. The clean 8730cfd
build-profile-edit repeat subsequently reached 68.0921% peak (30.7% average)
over 605.2 seconds with zero underruns/drops and six file cycles, and is the
accepted STAY result for the LFO workload. The clean 328a419 held-edit repeat
then reached 71.1283% peak (33.0% average) over 605.2 seconds with zero
underruns/drops, 386 live-edit cycles and five file cycles. It was REVIEW. The
subsequent clean `5466a96` filter-cache repeat reached 69.0852% peak (32.0%
average) over 605.2 seconds with zero underruns/drops and five file cycles;
it was the pre-filter-mode baseline. The clean `1a2eb5e` filter-mode repeat
reached 69.4887% peak (32.1% average) over 610.2 seconds and 122 helper
windows, with zero underruns/drops, 376 live-edit cycles and five pattern file
cycles. It is the accepted STAY result for the filter-mode implementation.
The clean `d891d3c` resonance repeat reached 69.9237% peak (32.8% average)
over 610.2 seconds and 122 helper windows, with zero underruns/drops, 376
live-edit cycles and five pattern file cycles. It is the accepted STAY result
for the resonance implementation and is near the 70% threshold. Each future
callback milestone requires a fresh full measurement. These runs do not close
the one-hour phase soak. The held-edit capture used the
clean `328a419` source and `build-profile-held` image SHA256
`d87b14a063c261b7395f5b7bb2f8f7c98e9f6ba45f66a2fffa3f111ff17d0c67`; full
provenance is in the recorded JSON metadata.

The filter-mode capture used clean source `1a2eb5e` and the
`build-profile-modes` image SHA256
`35aa620ee0aeadfa72cf0726b5a69fc2cc4e4afe996602fe5de32a9b9bd29c19`.
The helper reports 610.2 seconds and 122 windows; the capture metadata reports
607.85955 seconds of workload duration. Full provenance is in
`logs/perf-modes-wavex-20260912-123616.json`.

The final normal-image HIL passed both filter-mode preview/Apply/Revert/WXI
recall and held-note filter preview cases in 54.46 seconds; the final Filter
capture is `logs/filter-modes-20260912.png`.

Candidate 1 (`perf/dsp-1-filter`, commit `701aaab`) was rejected by its
controlled short trial. The `build-profile-dsp1` image SHA256 was
`3671abc926a1781512c07c623d62efe26e774add9a9c743d539230af115f4a0b`;
the interrupted capture ran 311.136 seconds over 62 windows, averaging
159,280 cycles and peaking at 340,745 cycles (70.9885%), versus the clean
baseline's 69.9396% peak. It had zero underruns/drops, 195 live edits and two
file cycles. The exact-output check covered 160 cases and matched the
2,027,520-byte baseline. This is rejected non-gating evidence; the helper did
not record a gate row because the trial was intentionally stopped below ten
minutes. Full capture metadata is in
`logs/perf-dsp1-wavex-20260912-232515.json`.

The resonance capture used clean source `d891d3c` and the
`build-profile-resonance` image SHA256
`cabc828e618c6faad759b30bc54977b7e4c38b3ee2355fc4b0616105353733b4`.
The helper reports 610.2 seconds and 122 windows; the capture metadata reports
607.679544 seconds of workload duration. Eight resonance routes were verified
in the capture metadata at voiceLFO2 → resonance depth 8000 with S-curve.
Full provenance is in `logs/perf-resonance-wavex-20260912-144145.json`.

The clean oscillator-pitch follow-up ran from source `6a58a7b` with the
`build-profile-osc-pitch` image SHA256
`ba026e56917a6ac46380ff053f37adc9b6d1ba94182da11e53a059f7593ba358`.
Its workload changes the per-Track matrix routes to LFO1 → OSC1_PITCH and
LFO2 → OSC2_PITCH, with route readback asserted; source provenance is also
retained in `/tmp/wavex-osc-pitch-source.json`. The helper row records the
accepted 69.9396% STAY result; the capture metadata reports 606.728292068
seconds of workload duration, with 376 live-edit cycles, five file cycles,
zero underruns/drops and 16 pitch routes verified. The exact capture files are
`logs/perf-osc-pitch-wavex-20260912-193020.log` and
`logs/perf-osc-pitch-wavex-20260912-193020.json`. This is near the 70%
threshold and does not close the one-hour phase soak. Final normal-firmware
HIL passed five selected pitch, resonance, filter-mode and held-note cases in
80.22 seconds (`/tmp/wavex-pitch-final-hil.log`).

Candidate 2 (`perf/dsp-2-setup`, commit `63cf37d`) was rejected by its clean
trial. The `build-profile-dsp2` image SHA256 was
`3233ba41f26d17e383b3b6f3fa5a42a6ff587964e52c06b70f1c043fd18662a6`;
the 607.454-second workload ran 121 windows with 164,388 average cycles and
339,017 peak cycles (70.6285%, REVIEW), versus the accepted 69.9396% baseline.
It had zero underruns/drops, 376 live-edit cycles and five file cycles. The
profiling zones measured events 11,303/146,105, modulation 29,294/73,014 and
render 117,674/177,404 cycles (average/peak). Host validation remained 613
tests with identical 2,027,520-byte output. The full helper row is recorded
below; this candidate is rejected and must not be adopted.

Candidate 3 (`perf/dsp-3-modulation`, commit `e581418d279ef91f6c8002d0ed9f086f926f5aa6`)
was also rejected. Its clean `build-profile-dsp3` image SHA256 was
`ba46e832b4c129d90970a0603bf8d1c5a6aeab526628111283c12c48a80c47d3`. The
first setup attempt (`logs/perf-dsp3-wavex-20260912-235334.json`) stopped before
capture because the baseline reported one dropped console RX byte; the same image was
rebooted and the retry completed successfully. The retry capture is
`logs/perf-dsp3-wavex-20260912-235623.log` with metadata in the matching JSON:
607.852755 seconds, 121 windows, 163,923 average cycles and 336,380 peak
cycles (70.0792%, REVIEW), versus the accepted 69.9396% baseline. It had zero
underruns/drops, 376 live-edit cycles and five file cycles. Profiling zones
measured events 11,159/140,877, modulation 26,270/68,831 and render
118,964/193,692 cycles (average/peak). Host validation remained 612 tests
with identical 2,027,520-byte output. The full helper row is recorded below;
this candidate is rejected and must not be adopted.

Candidate 4 (`perf/dsp-4-render`, commit `131313ab0df03c704258fdd967bbdbdb6efd74f0`)
was rejected as well. Its clean `build-profile-dsp4` image SHA256 was
`cd8e04fd1faa0108b9b78b167525c0daa660e89c0e3732928bfbaaed741b296d`. The
607.0647-second capture is
`logs/perf-dsp4-wavex-20260913-001252.log` with matching JSON metadata:
121 windows, 162,371 average cycles and 338,011 peak cycles (70.4190%, REVIEW),
with zero underruns/drops, 376 live-edit cycles and five file cycles. Profiling
zones measured events 9,551/149,014, modulation 25,378/70,139 and render
120,384/174,084 cycles (average/peak). Host validation remained 611 tests
with identical 2,027,520-byte output. The full helper row is recorded below;
this candidate is rejected and must not be adopted. The comparison is against
a single accepted baseline run, so it does not establish causality or separate
the change from run-to-run variation; repeat the unchanged baseline before
making further optimization decisions.

The unchanged original oscillator-pitch binary was then repeated twice to
measure baseline variation. Both runs used source commit
`6a58a7bbe5438691d0347e1755e2e78f89299e71`, the retained binary
`/tmp/wavex-original-baseline.bin`, and image SHA256
`ba026e56917a6ac46380ff053f37adc9b6d1ba94182da11e53a059f7593ba358` without
a rebuild. Repeat 1 is
`logs/perf-baseline-repeat1-wavex-20260913-002947.log` with matching JSON:
607.4224 seconds, average 159,767 cycles and peak 325,684 (67.8508%). Repeat
2 is `logs/perf-baseline-repeat2-wavex-20260913-004142.log` with matching JSON:
607.3339 seconds, average 159,765 and peak 336,606 (70.1262%, REVIEW). Both
had 376 live-edit cycles, five file cycles and zero underruns/drops; the
console reports RX dropped bytes, rather than audio-block or engine-command
drops. Event/modulation/render zones were 9,829/143,627, 25,560/73,299 and
117,596/186,863 for repeat 1, and 9,829/155,819, 25,566/69,647 and
117,591/176,365 for repeat 2 (average/peak). The first setup attempt,
`logs/perf-baseline-repeat1-wavex-20260913-002626.json`, reported one dropped
RX-byte count before timing and was retried successfully. The average span is
only eight cycles, but the peak span is 10,922 cycles (about 3.2%), so the
original near-70% result is not a robust single-run baseline.

The compiler/placement audit for this baseline found `-O2`, Cortex-M7 and hard
FPU flags on DSP objects, no LTO, and cold translation units. The callback is
at `0x900523c8` (0xe00 bytes); `EvaluateModMatrix` is QSPI at `0x90049ea0`
(0x344 bytes). VoiceManager render/trigger/live-apply/tick paths occupy about
19 KiB of the 64 KiB ITCM budget. Library `powf`, `tanf` and `arm_sin` remain
in QSPI. These facts are recorded to separate compiler and placement effects
from algorithm changes before the next candidate.

The profiling-control proof kept `src/profiling/profiler.cpp` at `-O2` for
profiling-enabled builds. The timing-control image
`build-profile-timing-control/wavex-daisy.bin` matched the original binary
SHA256 `ba026e56917a6ac46380ff053f37adc9b6d1ba94182da11e53a059f7593ba358`.
The profiler object SHA256 was identical across the original `-O2`, timing
control and `-O3` builds:
`9fb4c0a44933f739f13ff9a478f8d92fcb7cee32b17e793a15b3a895130d282b`.
The O3 image under test is `build-profile-o3-baseline`, SHA256
`8eba00eda23a0c288e9b98525802b7a3481d7f5106bd9a1c65d61796c4c65922`; no
runtime capacity claim is made until it is measured with this fixed profiler.

The adopted ITCM placement gate used source `6ae93efe142776b1c00ebc35ea07648946d8b450`,
image SHA256 `52fb21487fbc57a19175f5b19caa41955b9f750618ae01aed52553ce6b9ceb6f`,
and capture `logs/perf-itcm-control-wavex-20260913-011208.log` with matching
JSON metadata. Its 607.650711-second workload measured 148,248 average and
306,291 peak cycles (63.8106%, STAY), with 376 live edits, five file cycles,
zero audio underruns and zero console RX dropped bytes. The named ITCM
sections used 23,640 of 65,536 bytes (36.07%); host validation passed 611
tests with identical golden output. This is an accepted placement result, not
the one-hour Phase 2 soak.

After the gate, normal profiling-off O2 firmware was restored successfully;
the binary SHA256 was
`e0453c6a8fe12228bc1be2e87b1a98f34a280d7efdf952909b2b8a774c59b539`, with the
ITCM callback at `0x00004d94` (0xd50 bytes) and
`EvaluateModMatrix` at `0x000000ac` (0x344 bytes). The first HIL launch before
USB re-enumeration skipped five cases and is excluded. After an explicit Daisy
probe, the rerun passed all five selected matrix-destination, held-note and
filter-mode cases (80.12 seconds, 64 deselected) in
`/tmp/dsp-final-hil-retry.log`; this covers preview/revert/apply/save, held-note
editing and matrix destinations 5/6/7 preview/undo/WXI behavior. The full
one-hour Phase 2 soak remains open.

The fixed-profiler O3 trial was stopped intentionally after 188.205 seconds
and 38 windows, so it is diagnostic evidence only and has no full gate row.
The capture is `logs/perf-compiler-o3-wavex-20260913-010201.log` with matching
JSON metadata, using source `ff8248a00618036791f9bd385fa29ef2db9b0aca` and the
O3 image above. It completed 119 live edits and one file cycle with zero audio
underruns and zero console RX dropped bytes. Average callback cost was 168,248
cycles versus 159,765--159,773 in the unchanged repeats (about 5.3% higher);
peak cost was 336,660 versus the repeat maximum of 336,606, a small difference
within observed peak noise and not evidence of causality. Profiling zones were
events 10,359/146,623, modulation 21,506/64,420 and render 126,756/184,800
cycles (average/peak). Host validation passed 611 O3 Release tests with the
identical golden output. The O2 default remains unchanged; the O3 trial is
rejected for adoption pending a better-controlled experiment.

The accepted ITCM profiling-control image was then repeated twice more before
retesting the four optimization candidates. Both captures used source commit
`6ae93efe142776b1c00ebc35ea07648946d8b450`, image SHA256
`52fb21487fbc57a19175f5b19caa41955b9f750618ae01aed52553ce6b9ceb6f`, the
retained workload helper SHA256
`f3e1a168eb605ed5db8667b5f1bae197e7ed521308ab49983f75734c2f986e84`, and the
fixed profiler object SHA256
`9fb4c0a44933f739f13ff9a478f8d92fcb7cee32b17e793a15b3a895130d282b`.
Repeat 1 is `logs/perf-itcm-baseline-repeat1-wavex-20260913-043026.log` with
matching JSON metadata: 607.6513 seconds, average 148,248 cycles and peak
312,790 cycles (65.1646%). Repeat 2 is
`logs/perf-itcm-baseline-repeat2-wavex-20260913-044230.log` with matching JSON:
607.6060 seconds, average 148,233 cycles and peak 308,425 cycles (64.2552%).
Both completed 376 live-edit cycles and five file cycles with eight voices,
zero audio underruns and zero console RX dropped bytes.
Profiling zones were events 6,791/145,639, modulation 19,162/52,800 and
render 118,130/177,425 cycles for repeat 1, and events 6,790/148,837,
modulation 19,162/51,681 and render 118,118/186,503 for repeat 2
(average/peak). Together with the original accepted run (148,248 average,
306,291 peak), the three-run average range is 148,233--148,248 cycles (15
cycles, 0.0101%); the peak range is 306,291--312,790 (6,499 cycles, 2.12%).
The failed setup attempt `logs/perf-itcm-baseline-repeat1-wavex-20260913-042753.json`
reported three console RX dropped bytes before timing and produced no timed
capture, so it is excluded from gate evidence. The successful retry included
10 seconds of post-flash settling outside the workload. This establishes a
stable unchanged-image baseline for the four candidate comparisons. Fresh host
validation passed all 611 Daisy tests in 3.15 seconds, with the unchanged
2,027,520-byte output fingerprint SHA256
`f4834359cff7f37bb97790d6feb9419bc5c6495bc21967bd9bf103eeb672957a`.

Candidate 1 (cutoff prewarp cache, source
`26132d4b507f431af25a0caa08aa8ef4e85daa83`) was retested twice after ITCM
placement. Both used candidate image SHA256
`b33f13f08ed8dd365813ab8a483eb95b988d837609f6ea4ca415d69e1abeb2a2`,
the unchanged O2 profiler, and the baseline workload. Captures
`logs/perf-itcm-dsp1-wavex-20260913-050343.log` and
`logs/perf-itcm-dsp1-repeat-wavex-20260913-051604.log` have matching JSON
metadata: 607.3940 and 607.4261 seconds, 376 live-edit cycles and five file
cycles each, with zero audio underruns and zero console RX dropped bytes.
Their average/peak callback costs were 146,054/315,354 and 146,075/316,824
cycles. The average gain of 1.46--1.48% repeated, but both observed peaks
exceeded the unchanged baseline's worst peak of 312,790 cycles, by 0.82%
and 1.29%. Both capacity results are **STAY**; the optimization is nevertheless
not adopted because this headroom-focused comparison does not justify trading
higher observed peaks for lower average cost. This is a conservative selection,
not statistical proof of a worst-case regression. ITCM grew by 704 bytes to
24,344 bytes. All 612 host tests and the exact-output fingerprint passed.

Candidate 2 (envelope/LFO setup caches, source
`79802233dd806733956907c089e788d2dd96a764`) used image SHA256
`9f552fd6350a9103eb2a725636c2198919849fab9058c20b0e45084afa8e161f`
with the same profiler and unchanged ITCM baseline comparison. Capture
`logs/perf-itcm-dsp2-wavex-20260913-053214.log` and its JSON metadata
record 607.5871 seconds, 376 live-edit cycles, five file cycles, zero audio
underruns and zero console RX dropped bytes. Callback cost was 151,111 average
and 316,707 peak cycles: roughly 1.93% more average work and a 1.25% higher
observed peak than the baseline's maximum. Events measured 8,174/153,088,
modulation 19,934/50,853 and rendering 118,624/181,482 cycles (average/peak).
Capacity remains **STAY**; the optimization is not adopted. ITCM used
23,920 bytes, 280 more than baseline. All 613 host tests and the unchanged
2,027,520-byte output fingerprint passed. No candidate has changed the accepted
firmware baseline at this point.

The dirty filter-cache retry used the same held-edit workload before the
follow-up clean commit: `build-profile-filter-cache`, image SHA256
`e598775059836ad6a95cc0675a6fcf10b9d9e3ecfde4cdd54acffca1fb96a91f`,
125.032 seconds over 25 windows, 153,323 average cycles and 306,410 peak
cycles (63.8354%), with zero underruns/drops, 80 live-edit cycles and one file
cycle. This is short diagnostic evidence only; the clean 328a419 result at
71.1283% is historical REVIEW evidence. The 5466a96 result is the pre-mode
baseline, the 1a2eb5e result is the pre-resonance baseline, and the d891d3c
result is the pre-pitch baseline; all are superseded as the latest full result
by the accepted 6a58a7b gate.

Candidate 3 (modulation exponent cache, source
`af2804eb160a7d021b9d5315fa24ce28a5745ae5`) is adopted after two full
matched captures of image SHA256
`4b1268c4ec656039017a0240c24757ea0ef16d7de5428095047dbae5b76e914d`.
The O2 profiler object and workload helper hashes remain unchanged.
`logs/perf-itcm-dsp3-wavex-20260913-054739.log` and
`logs/perf-itcm-dsp3-repeat-wavex-20260913-060007.log` have matching JSON
metadata, recording 607.6649 and 607.1274 seconds of workload. Both completed
376 live-edit cycles and five file cycles with zero audio underruns and zero
console RX dropped bytes. Average/peak callback costs were 147,942/312,215
and 147,957/310,493 cycles. The average reduction is about 0.2%, larger than
the observed unchanged-image average range; peaks remain inside the original
baseline range, so no peak-headroom improvement is claimed. Both capacity
results are **STAY**.

The candidate has 122 five-second profiler windows per capture, versus 121
in the original baseline captures; the helper's summed window time includes
capture edges and differs from workload elapsed time. Excluding the first
and last windows, and then two windows at each end, preserves the average
gain. The exact calculations are retained in
`logs/perf-itcm-edge-window-check.json`. The cache costs 32 bytes per
voice (256 bytes for eight voices); ITCM grows by 288 bytes to 23,928 bytes.
All 612 host tests and the unchanged exact-output fingerprint passed. The two
candidate captures establish the new accepted comparison range for candidate 4:
147,942--147,957 average cycles and 310,493--312,215 peak cycles. These short
repeats do not replace the one-hour Phase 2 soak.

Candidate 4 (specialized rendering loops, source
`59783fec3a22296b68081d14c7e4af3202112103`) was measured against the
newly accepted modulation-cache pair, not the older pre-cache baseline.
Image SHA256
`dd0f74de577f5e826d9f6d8448a7ea1df01cf18f528c9eee82bc8670532ed857`
retained the O2 profiler and the accepted ITCM placement and modulation cache.
`logs/perf-itcm-dsp4-wavex-20260913-061613.json` and its matching log
record 607.7122 seconds, 376 live-edit cycles, five file cycles, zero audio
underruns and zero console RX dropped bytes. Average/peak callback cost was
151,174/317,518 cycles: about 2.18% more average work and a 1.70% higher peak
than the accepted pair's worst values. Events measured 7,759/147,039,
modulation 18,551/52,856 and rendering 120,134/179,585 cycles (average/peak).
Capacity is **STAY**, but the optimization is not adopted. ITCM grew by
1,048 bytes to 24,976 bytes. All 612 host tests and the unchanged exact-output
fingerprint passed.

All four candidates have now been retested in order after the repeated ITCM
baseline. Only the modulation cache is adopted; candidate 4 includes that
accepted change for its comparison. Candidates 1, 2 and 4 remain isolated on
their experiment branches. The accepted profiling image remains the two-run
modulation-cache reference above, with a worst observed callback of 312,215
cycles (65.0448%, STAY). The one-hour Phase 2 soak and full phase gate remain
open.

Normal profiling-off O2 firmware was then rebuilt and restored from the
accepted source on `develop` (`251a96b`; firmware code `af2804e`).
Its binary SHA256 is
`87a94650bcd3125f1cbab31e09d71379078ba2ab6a67ea3472682c7533b35bdb`.
The callback remains at `0x00004eb4` (0xd50 bytes), and the modulation
evaluator at `0x000000ac` (0x43e bytes); normal ITCM usage is 23,720 bytes.
After USB readiness was confirmed, all five selected two-board HIL cases
passed in 80.60 seconds (64 deselected, none skipped): held-note preview,
Apply/Revert, filter-mode preview/save, and matrix destinations 5/6/7 with
preview/undo/WXI recall. The container log is `/tmp/itcm-final-hil.log`.
The accepted source retains the modulation cache and excludes the three
rejected code changes. The exact unchanged benchmark script is retained
locally as `logs/perf-itcm-workload.py` under its baseline SHA256 above.

## Recorded runs

| Date | Commit | Scenario | Voices | Hz/block | Core | Image | Duration | Budget cycles | Average cycles | Maximum cycles | Worst headroom | Stream underruns | Callback features left | Decision | Note |
|---|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---|---|---|
| 2026-09-07 | 176ce1f | 8 voices; WaveX 24 dB; drive 100%; 8 mod slots; sequencer; SD stream; cutoff updates | 8 | 48000/48 | 480 MHz | qspi `-O2` | 3606.0s (721 windows) | 480000 | 125013 (26.0%) | 315854 (65.8029%) | 34.1971% | 0 | yes | STAY | One-hour soak; zero startup/runtime underruns; console-triggered notes; locks stored but not applied. Capture: perf-wavex-20260907-212459.log |
| 2026-09-07 | 176ce1f | 8 voices; DaisySP SVF; drive 100%; 8 mod slots; sequencer; SD stream; cutoff updates | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 125695 (26.2%) | 430385 (89.6635%) | 10.3365% | 0 | yes | UPGRADE | Capacity gate blocked; zero startup/runtime underruns; slope argument ignored by DaisySP. Capture: perf-daisysp-20260907-222524.log |
| 2026-09-07 | 66d0330 | 8 Tracks; WaveX 24 dB; drive 100%; 8 mod slots per Track; sequencer; SD stream; cutoff updates | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 119331 (24.9%) | 318476 (66.3492%) | 33.6508% | 0 | yes | STAY | Track-addressed follow-up; zero startup/runtime underruns and dropped commands; locks stored but not applied. Capture: perf-wavex-20260908-003427.log |
| 2026-09-07 | 271c8e3 | 8 kits; Track-local choke; 64 mod slots; WaveX 24 dB drive 100%; sequencer; SD stream; touch grid readback | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 123172 (25.7%) | 322005 (67.0844%) | 32.9156% | 0 | yes | STAY | Ten-minute kit/grid follow-up; zero startup/runtime underruns and dropped commands; one-shot hits every second step; locks stored but not applied. Capture: perf-wavex-20260908-023420.log |
| 2026-09-07 | 664c6d3 | 8 kit voices, note/velocity prepared zones, 24 dB WaveX drive, 8 mod slots, SD stream, touch grid | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 123900 (25.8%) | 337694 (70.3529%) | 29.6471% | 0 | yes | REVIEW | 605 s clean-commit note/velocity resolver run; eight voices, one stream, zero dropped events; metadata logs/perf-wavex-20260908-034526.json |
| 2026-09-08 | 00fdd08 | 8 kit voices, sparse prepared-zone publication, 24 dB WaveX drive, 8 mod slots, SD stream, touch grid | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 120465 (25.1%) | 331758 (69.1162%) | 30.8838% | 0 | yes | STAY | 605 s clean-commit sparse publication comparison; eight voices, one stream, zero dropped events; metadata logs/perf-wavex-20260908-040257.json |
| 2026-09-08 | 95ac09b | 8 Tracks, 16 populated pads per kit, WaveX 24 dB full drive, 64 mod slots, SD 25MHz stream, touch grid, no file operations | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 115946 (24.2%) | 337098 (70.2288%) | 29.7712% | 0 | yes | REVIEW | 605 s audio/grid-only run; zero underruns and sequencer queue-drop messages; persistence directory recovery pending; metadata logs/perf-wavex-20260908-053146.json |
| 2026-09-08 | 38b83b0 | 8 Tracks, indexed 16-pad kits, WaveX 24 dB full drive, 64 mod slots, SD 25MHz stream, touch grid, no file operations | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 118946 (24.8%) | 337649 (70.3435%) | 29.6565% | 0 | yes | REVIEW | 605 s indexed-pad comparison; zero underruns and sequencer queue-drop messages; full persistence gate blocked; metadata logs/perf-wavex-20260908-054919.json |
| 2026-09-08 | 70deebd | 8 Tracks, 16-pad kits, combined WaveX filter retune, 24 dB full drive, 64 mod slots, SD 25MHz stream, touch grid, no file operations | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 124071 (25.8%) | 328090 (68.3521%) | 31.6479% | 0 | yes | STAY | 605 s combined-retune comparison; zero underruns and sequencer queue-drop messages; full persistence soak remains blocked by original directory; metadata logs/perf-wavex-20260908-060531.json |
| 2026-09-08 | 27b7fd6 | 8 Tracks, 16-pad kits, WaveX 24 dB full drive, 64 mod slots, SD 25MHz stream, touch grid, repeated pattern save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 123978 (25.8%) | 330933 (68.9444%) | 31.0556% | 0 | yes | STAY | 605 s persistence run after authorized directory preservation/recreation; six save/load cycles; zero underruns, sequencer queue-drop messages and file errors; metadata logs/perf-wavex-20260908-102335.json |
| 2026-09-10 | 3e415cf | 8 Tracks,16-pad kits,pad sound overrides,WaveX24dB full drive,64mod slots,live Instrument/pad cutoff,SD25MHz stream,grid,pattern save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 124555 (25.9%) | 339751 (70.7815%) | 29.2185% | 0 | yes | REVIEW | 605 s pad-sound baseline; six save/load cycles; zero underruns, sequencer queue drops and file errors; metadata logs/perf-wavex-20260911-011618.json |
| 2026-09-10 | f904032 | 8 Tracks,16-pad kits,pad sound overrides,combined voice filter tuning,WaveX24dB full drive,64mod slots,live Instrument/pad cutoff,SD25MHz stream,grid,pattern save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 123316 (25.7%) | 333170 (69.4104%) | 30.5896% | 0 | yes | STAY | 605 s matched combined-tuning comparison; six save/load cycles; zero underruns, sequencer queue drops and file errors; metadata logs/perf-wavex-20260911-013012.json |
| 2026-09-12 | 9dc8498 | 8 Tracks; 16-pad kits; 8 mod slots; 24 dB drive; grid; SD stream; live cutoff; pattern files | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 122000 (25.4%) | 336230 (70.0479%) | 29.9521% | 0 | yes | REVIEW | Clean pre-lock image captured before source edits; 610.2 s; image hash and workload in adjacent JSON. Fresh REVIEW supersedes prior STAY. |
| 2026-09-12 | 9dc8498+ | 8 Tracks; four applied locks per hit; 8 mod slots; 24 dB drive; grid; SD stream; live cutoff; pattern files | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 137231 (28.6%) | 352288 (73.3933%) | 26.6067% | 0 | yes | REVIEW | Diagnostic lock implementation before moving DSP initialization to startup; image/workload in adjacent JSON; no clean-commit gate claim. |
| 2026-09-12 | 3dee3bf | 8 voices; four applied locks per hit; WaveX 24dB full drive; 64 mod slots; SD streaming; live Track/pad edits; grid; periodic pattern save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 142765 (29.7%) | 334546 (69.6971%) | 30.3029% | 0 | yes | STAY | Clean 3dee3bf, identical audio image to 5d9683a; startup DSP initialization; eight voices checked after every file-load restart; stopped one-shot voices may finish during load. Capture JSON records image SHA and six file cycles. Earlier 004704 run failed a benchmark voice-count assertion during stopped transport and is not a gate result. |
| 2026-09-12 | 409e40c | Two source maps, 8 Tracks x 16 pads, 64 mod slots, 4 locks per hit, WaveX 24dB/full drive, live cutoff, SD stream, grid, pattern file cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 151643 (31.6%) | 315322 (65.6921%) | 34.3079% | 0 | yes | STAY | Clean source at capture start; image d020cd6f400c29b10bc01c751f2f4b3865c021533b2dd24ebe42757b3d7bc0b9; full two-source gate; metadata logs/perf-dual-wavex-20260912-020336.json |
| 2026-09-12 | db6f1805e31b82aa61f860721d3be6cd621115e5 | 8 Tracks, 2x16 zones, Env 1-3 and 64 live routes, 4 locks per hit, WaveX 24 dB full drive, stream/grid/file cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 154075 (32.1%) | 321394 (66.9571%) | 33.0429% | 0 | yes | STAY | Clean source at capture; image SHA256 bf8bd1929024a7a6988d9d68fde64f34151b6e677f17d900edcb9a781023a20e; logs/perf-env-wavex-20260912-025245.json records all maps, envelopes, routes and six file cycles. Frontend-only edits during the fixed-image run. |
| 2026-09-12 | 6ad289694be8590ec53d9e7bb67800d41e90a537 | Eight voices; sixteen voice-owned sine LFOs at20Hz; two sixteen-zone maps per Track;64routes;four locks per hit;WaveX24dB full drive;SDstream;live cutoff;touchgrid;periodic files | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 169743 (35.4%) | 349837 (72.8827%) | 27.1173% | 0 | yes | REVIEW | Clean source image build-profile-events, SHA256 744c05c6c99ef0393857bc5d34e1254e347cdcbf26b2d92b8addb9328b149cd4. Retried after USB reenumeration using the retained clean pre-flash snapshot; source edits resumed after flashing while the image stayed unchanged. Metadata logs/perf-lfo-wavex-20260912-035853.json records provenance, LFO readbacks, voice counts and file cycles. |
| 2026-09-12 | 8730cfd49a349dd265014baaaa55137d72df38ec | 8 Tracks; sixteen populated pads per kit; step notes 60-75; 8 mod slots per Instrument; four applied locks (cutoff/resonance/pan/pitch) per enabled step; WaveX 24 dB drive 100%; sequencing; SD stream; cutoff updates on all 8 Tracks; per-pad sound overrides on pads 1/5/9/13, live per-pad cutoff edits; eight drum Instruments; sixteen populated pads with choke group 1 per Track; one-shot at every second step; periodic save/load; streaming preview stops for file operations and restarts afterward; touch sequencer grid active with 16-step readback and 25 Hz playhead; during stopped file-load transitions one-shot voices may finish naturally; eight voices required after each restart; two 16-zone oscillator maps per Track with distinct kick PCM; 50/50 mono submix; Oscillator 2 detuned +17 cents; three active envelope sources with independent Env2/Env3 timing; all 64 routes use available sources; sixteen voice-owned sine LFOs at 20 Hz, gate retrigger with 1 ms delay and 3 ms fade; CMSIS sine kernel | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 147323 (30.7%) | 326842 (68.0921%) | 31.9079% | 0 | yes | STAY | Clean 8730cfd source captured before flash; build-profile-edit image SHA256 81dc208fdccb0dd2a5c78e4b3ea7b69257dbc00c0bcd4d64b4f552d10fe5e64d; metadata logs/perf-lfo-wavex-20260912-043416.json; frontend LFO page source edits continued after capture, flashed firmware unchanged. Identical two-source /16 sine LFO workload; ITCM VoiceLfo::Start and TickModulation placement. Zero underruns/drops; six successful pattern file cycles. |
| 2026-09-12 | 328a4193fce09de9070f2f76642705233b0487eb | 8 voices; held Instrument sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 158319 (33.0%) | 341416 (71.1283%) | 28.8717% | 0 | yes | REVIEW | Clean held-note callback checkpoint; REVIEW requires callback optimization before further scope |
| 2026-09-12 | 5466a96bd01b6c1af5c161fe56ad824904e56471 | 8 voices; held Instrument sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 153509 (32.0%) | 331609 (69.0852%) | 30.9148% | 0 | yes | STAY | Clean filter snapshot cache repeat; image SHA256 e598775059836ad6a95cc0675a6fcf10b9d9e3ecfde4cdd54acffca1fb96a91f; capture logs/perf-held-wavex-20260912-053400.log and JSON metadata; five file cycles, 386 live-edit cycles; accepted STAY, one-hour phase soak remains open |
| 2026-09-12 | 1a2eb5ee714e78d5cc89cdbdde4ec290f7eb6341 | 8 Tracks; two 16-zone oscillator maps; 64 routes; four locks per hit; WaveX 24 dB drive 100%; sixteen sine LFOs at 20 Hz; Env 1-3; SD stream; touch grid 25 Hz; all Tracks HP then alternating HP/Notch live filter edits on rotating Tracks | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 154101 (32.1%) | 333546 (69.4887%) | 30.5113% | 0 | yes | STAY | Clean filter-mode gate; accepted STAY, one-hour phase soak and polyphony gate remain open |
| 2026-09-12 | d891d3c7ed2ee95991910c806db97bf41ed5e4ec | 8 Tracks; two 16-zone oscillator maps; 64 routes; four locks per hit; WaveX 24 dB full drive; sixteen sine LFOs at 20 Hz; Env 1-3; SD stream; touch grid 25 Hz; HP/Notch filter modes; voiceLFO2 to resonance depth 8000 S-curve on slot 8 of each Track | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 157210 (32.8%) | 335634 (69.9237%) | 30.0763% | 0 | yes | STAY | Clean resonance matrix gate; accepted STAY, near-70% threshold; one-hour phase soak remains open |
| 2026-09-12 | 6a58a7bbe5438691d0347e1755e2e78f89299e71 | 8 Tracks; two 16-zone oscillator maps; 64 routes; four locks per hit; WaveX 24 dB full drive; sixteen sine LFOs at 20 Hz; Env 1-3; SD stream; touch grid 25 Hz; HP/Notch filter modes; LFO1 to OSC1_PITCH and LFO2 to OSC2_PITCH on every Track | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 159773 (33.3%) | 335710 (69.9396%) | 30.0604% | 0 | yes | STAY | Clean oscillator-pitch gate; accepted STAY near 70%; one-hour phase soak remains open |
| 2026-09-12 | 63cf37dca45e39ed6e511077b36fdf8aed46bb73 | 8 voices; held Instrument sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 164388 (34.2%) | 339017 (70.6285%) | 29.3715% | 0 | yes | REVIEW | Rejected candidate 2; clean setup-cache trial is REVIEW versus accepted 69.9396% baseline; do not adopt |
| 2026-09-13 | e581418d279ef91f6c8002d0ed9f086f926f5aa6 | 8 voices; held Instrument sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load; four cached modulation mappings | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 163923 (34.2%) | 336380 (70.0792%) | 29.9208% | 0 | yes | REVIEW | Rejected candidate 3; clean modulation-cache trial is REVIEW versus accepted 69.9396% baseline; do not adopt |
| 2026-09-13 | 131313ab0df03c704258fdd967bbdbdb6efd74f0 | 8 voices; held Instrument sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load; specialized single/dual-source render loops | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 162371 (33.8%) | 338011 (70.4190%) | 29.5810% | 0 | yes | REVIEW | Rejected candidate 4; clean render-loop specialization trial is REVIEW versus accepted 69.9396% baseline; do not adopt; single-baseline comparison |
| 2026-09-13 | 6a58a7bbe5438691d0347e1755e2e78f89299e71 | Unchanged accepted oscillator-pitch baseline repeat 1; 8 voices; held sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 159767 (33.3%) | 325684 (67.8508%) | 32.1492% | 0 | yes | STAY | Unchanged baseline repeat 1; zero underruns/drops and console RX dropped bytes 0; original binary retained without rebuild |
| 2026-09-13 | 6a58a7bbe5438691d0347e1755e2e78f89299e71 | Unchanged accepted oscillator-pitch baseline repeat 2; 8 voices; held sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 159765 (33.3%) | 336606 (70.1262%) | 29.8738% | 0 | yes | REVIEW | Unchanged baseline repeat 2; zero underruns/drops and console RX dropped bytes 0; original binary retained without rebuild |
| 2026-09-13 | 6ae93efe142776b1c00ebc35ea07648946d8b450 | Unchanged accepted ITCM profiling-control baseline repeat 1; 8 voices; held sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 148248 (30.9%) | 312790 (65.1646%) | 34.8354% | 0 | yes | STAY | Same accepted image SHA256 `52fb2148…`; 376 live edits, five file cycles, zero audio underruns and console RX dropped bytes 0; metadata `logs/perf-itcm-baseline-repeat1-wavex-20260913-043026.json` |
| 2026-09-13 | 6ae93efe142776b1c00ebc35ea07648946d8b450 | Unchanged accepted ITCM profiling-control baseline repeat 2; 8 voices; held sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 148233 (30.9%) | 308425 (64.2552%) | 35.7448% | 0 | yes | STAY | Same accepted image SHA256 `52fb2148…`; 376 live edits, five file cycles, zero audio underruns and console RX dropped bytes 0; metadata `logs/perf-itcm-baseline-repeat2-wavex-20260913-044230.json` |
| 2026-09-13 | 6ae93efe142776b1c00ebc35ea07648946d8b450 | 8 voices; held Instrument sound edits; two oscillator maps; Env 1-3; two per-voice LFOs; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; periodic save/load; Callback and modulation evaluation in named ITCM | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 148248 (30.9%) | 306291 (63.8106%) | 36.1894% | 0 | yes | STAY | Accepted ITCM placement gate; clean full run with fixed O2 profiler, zero audio underruns and zero console RX dropped bytes; ITCM placement adopted; one-hour phase soak remains open |
| 2026-09-13 | 26132d4b507f431af25a0caa08aa8ef4e85daa83 | 8 voices; two oscillator maps; Env 1-3; two LFOs per voice; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; live edits; grid; periodic save/load; Cutoff cache trial 1 | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 146054 (30.4%) | 315354 (65.6988%) | 34.3012% | 0 | yes | STAY | Cutoff cache trial 1; not adopted; 376 live edits and five file cycles; zero audio underruns and console RX dropped bytes; metadata logs/perf-itcm-dsp1-wavex-20260913-050343.json |
| 2026-09-13 | 26132d4b507f431af25a0caa08aa8ef4e85daa83 | 8 voices; two oscillator maps; Env 1-3; two LFOs per voice; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; live edits; grid; periodic save/load; Cutoff cache trial 2 | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 146075 (30.4%) | 316824 (66.0050%) | 33.9950% | 0 | yes | STAY | Cutoff cache trial 2; not adopted; 376 live edits and five file cycles; zero audio underruns and console RX dropped bytes; metadata logs/perf-itcm-dsp1-repeat-wavex-20260913-051604.json |
| 2026-09-13 | 79802233dd806733956907c089e788d2dd96a764 | 8 voices; two oscillator maps; Env 1-3; two LFOs per voice; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; live edits; grid; periodic save/load; Envelope/LFO setup cache | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 151111 (31.5%) | 316707 (65.9806%) | 34.0194% | 0 | yes | STAY | Envelope/LFO setup cache; not adopted; 376 live edits and five file cycles; zero audio underruns and console RX dropped bytes; metadata logs/perf-itcm-dsp2-wavex-20260913-053214.json |
| 2026-09-13 | af2804eb160a7d021b9d5315fa24ce28a5745ae5 | 8 voices; two oscillator maps; Env 1-3; two LFOs per voice; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; live edits; grid; periodic save/load; modulation exponent cache | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 147942 (30.8%) | 312215 (65.0448%) | 34.9552% | 0 | yes | STAY | Modulation cache trial 1; adopted average gain, no peak gain claimed; 376 live edits and five file cycles; zero audio underruns and console RX dropped bytes; metadata logs/perf-itcm-dsp3-wavex-20260913-054739.json |
| 2026-09-13 | af2804eb160a7d021b9d5315fa24ce28a5745ae5 | 8 voices; two oscillator maps; Env 1-3; two LFOs per voice; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; live edits; grid; periodic save/load; modulation exponent cache | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 147957 (30.8%) | 310493 (64.6860%) | 35.3140% | 0 | yes | STAY | Modulation cache trial 2; adopted average gain, no peak gain claimed; 376 live edits and five file cycles; zero audio underruns and console RX dropped bytes; metadata logs/perf-itcm-dsp3-repeat-wavex-20260913-060007.json |
| 2026-09-13 | 59783fec3a22296b68081d14c7e4af3202112103 | 8 voices; two oscillator maps; Env 1-3; two LFOs per voice; 64 routes; four locks per hit; WaveX 24 dB full drive; SD stream; live edits; grid; periodic save/load; accepted modulation cache plus specialized render loops | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 151174 (31.5%) | 317518 (66.1496%) | 33.8504% | 0 | yes | STAY | Not adopted; higher average and peak than accepted modulation-cache pair; 376 live edits, five file cycles, zero audio underruns and console RX dropped bytes; metadata logs/perf-itcm-dsp4-wavex-20260913-061613.json |
