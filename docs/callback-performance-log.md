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
