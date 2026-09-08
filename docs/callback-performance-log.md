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
The production path remains `wavex/patterns`. No original directory was
renamed or deleted; preserving/recreating it awaits user authorization.
The temporary listing and rename instrumentation was removed. A retained
visible-file restart check passed after backend restart (`logs/pattern-reboot-
check.json`), preserving note, groove, tempo and Track bindings. The full
persistence soak and new-save recovery remain blocked by the original
directory failure; the short fresh-directory run does not satisfy the
ten-minute gate.

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
