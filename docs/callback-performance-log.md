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

## Same-frame admission batching — 2026-09-20

The 16-Track/four-layer deadline failure below exposed work for notes stolen
before rendering: 64 full parameter resolutions, lock applications and DSP
starts for eight surviving channels. The callback now selects compact prepared
zone indices, replays whole-note admission/choke decisions in order, and
materializes only surviving layers. Batches stop at each sample-offset boundary.
Rejected notes remain side-effect-free; superseded admissions still advance
note identities, ages and random seeds exactly as the sequential path does.

Persistent QSPI `-O2`, profiling On/detail Off/RTT Off candidate SHA256:
`ac4f254f5d2b1b3570ab37da041701a36cac47a9d79b11fde7ece401002cb736`.
Built from `dbd1ebf+` before the storage commit; frontend remains
`82fbc61818891bfaf1885f52213e6a67d09c277f3c7a8a21761a0bcb16b38a1a`.
The same 16-Track/four-layer Mono preset, both oscillators, full ladder,
modulation, four locks, live edits and stereo audition produced:

| Capture | DWT duration | Average | Maximum | Pattern cycles | Stream underruns |
|---|---:|---:|---:|---:|---:|
| Initial screen | 40.01 s | 35.57% | 58.0788% | 0 | 0 |
| Extended pressure | 605.86 s | 35.35% | 66.0025% | 10 | 0 |

The extended maximum is 316,812 / 480,000 cycles. The boot/setup-inclusive
capture peaks at 320,827 cycles (66.8390%). The previous image exceeded
100% in the same pressure preset (114.5108% playback / 118.0392% with setup).
This closes that reproduced deadline failure for the tested preset; it does
not clear the earlier 74.5896% mixed-channel transition or the complete phase
gate. Preserve the original failure and repeat the full mixed-channel soak
on the final image. Broader live-MIDI coincidence, physical MIDI latency,
listening and saved polyphony controls remain separate checks.

Evidence: `logs/stereo-0-ladder-20260920-144239.log` / `.json`,
`logs/stereo-0-ladder-20260920-144554.log` / `.json`, and
`logs/capacity-batch-pressure-full.log`. All sampled console RX-drop counts
were zero. Host validation: 779 Daisy tests, including sequential-versus-batch
state/audio comparisons across 1,600 notes and scheduler offset checks;
22 targeted address/undefined-behavior sanitizer cases pass. These checks do
not substitute for listening or a one-hour final-image soak.

**Combined MIDI screen:** an 80.024-second run on the same image added 68
foreground MIDI-channel injections fanning out to all 16 four-layer Tracks,
plus one Pattern save/load. Average was 35.4688%, peak 312,225 cycles
(65.0469%), and stream underruns/console RX drops remained zero. However,
`logs/stereo-0-ladder-20260920-145821.log` / `.json` contains **540 whole-note
queue refusals**. The bounded queue protects callback time by refusing complete
notes; this is not evidence that all routed MIDI notes sound. Keep admission
pressure in the held-key/routing work and distinguish it from console RX drops.
This short screen is not a ten-minute gate row or a physical MIDI test.

**Rapid mixed-channel transitions:** the same batching image then rotated
through all five full-budget stereo/Mono mixes every ten seconds, with both
oscillators, ladder, modulation, locks, streaming, live edits and ten Pattern
save/load cycles. The 121 DWT windows span 605.2 seconds: average 145,494 cycles
(30.3113%), maximum 287,832 (59.9650%), zero stream underruns and sampled console
RX drops. Evidence: `logs/stereo-0-ladder-20260920-150155.log` / `.json`.
This targeted repeat did not reproduce the earlier 74.5896% transition peak;
it does not replace the one-hour soak on the final image.

## Full-channel soak and transition peak — 2026-09-20

The recovery image completed 3,667.23 seconds of the mixed workload and 61
Pattern save/load cycles, with zero stream underruns and sampled console RX
drops. Both oscillators, full-drive ladder, three envelopes, two sine LFOs,
eight modulation routes, four locks per hit, sequencing, stereo SD audition
and live filter edits were active. All five channel mixes ran for more than
ten minutes, followed by a return to eight Mono voices. Source: `dbd1ebf+`;
Daisy profiling/detail-Off/RTT-Off SHA256
`bbb29c9e2ef70a4708829871344ce5076c777f6c7a568b0670cd0d58146495ac`;
ESP32 SHA256
`82fbc61818891bfaf1885f52213e6a67d09c277f3c7a8a21761a0bcb16b38a1a`.

| Segment | DWT duration | Average callback | Maximum callback |
|---|---:|---:|---:|
| Eight Mono | 610.19 s | 35.31% | 65.56% |
| One stereo + six Mono | 610.19 s | 32.84% | 59.50% |
| Two stereo + four Mono | 610.20 s | 30.37% | 56.38% |
| Three stereo + two Mono | 610.19 s | 27.86% | 51.18% |
| Four stereo | 610.18 s | 25.36% | 49.18% |
| Return to eight Mono | 610.19 s | 35.27% | 64.85% |
| Final switch to one stereo, with Pattern reload | 5.00 s | 33.30% | **74.59%** |

The final requested seconds crossed another mix boundary at 3,660 seconds,
coinciding with the 61st file cycle. This last window contains the overall
maximum of **358,030 cycles / 480,000 (74.5896%, REVIEW)**. Across all 733
windows / 3,666.13 seconds, weighted average was 149,634 cycles (31.1738%).
The workflow passed; the callback capacity gate did not. Keep the final window
in the result rather than trimming the run to its lower steady peaks. The
ordinary event/render window maxima do not identify coincident stage costs.

Evidence: `logs/stereo-0-ladder-20260920-043125.log` / `.json`, per-segment
`-mix0.log` through `-mix6.log`, `logs/capacity-all-mixes-full.log` and
`logs/capacity-all-mixes-esp32.log`. Segment boundaries include transition
windows and are not isolated optimization comparisons. The boot/setup-inclusive
maximum is also 74.5896%. The last periodic Daisy UART health record has zero
CRC, sync, sequence drop and RX-overflow counts; 7,558 TX-full rejections are
reported separately.
The captured frontend log has no hardware FIFO-overflow report. This run does
not establish physical MIDI timing, interrupt-entry latency, panel behavior or
listening acceptance, and it is not a clean-commit repeat.

**Detailed follow-up:** A separate instrumented image
`b07430d430c4cb72a5f06ea664af077c4ec7ee6737bf69e19c7e60fbd9eb0880`
repeated mix changes every ten seconds for 187.83 seconds: 18 transitions,
three Pattern cycles and zero stream underruns/console RX drops. It did not
reproduce the original high window. Its boot/setup-inclusive maximum was
316,431 cycles (65.9231%), comprising 141,714 render, 49,151 sequencer-trigger,
24,634 sequencer-command, 23,268 modulation, 22,569 resolve, 15,420 tick,
10,474 lock, 5,923 control and 331 queue cycles, plus unclassified callback
work. Voice initialization is nested inside trigger cost (35,213 cycles for
eight starts), not another additive stage. These measurements identify costs
in that diagnostic peak, not the cause of the original 74.5896% event.
No speculative audio change or raised threshold was accepted. Detailed
instrumentation is excluded from gate rows; the original REVIEW still applies.
Evidence: `logs/stereo-0-ladder-20260920-053920.log` / `.json` and
`logs/capacity-transition-detail-full.log`.

### Layered and repeated-note runs

The ordinary recovery image then completed both full ten-minute layer runs.
Each used the same DSP/streaming/live-edit workload and ten Pattern save/load
cycles, with four overlapping zones per note and both oscillators. Repeated
MIDI-channel notes were injected through the Daisy console's real foreground
routing/queue path alongside the sequencer. These are not physical DIN/USB
input or latency tests.

| Scenario | DWT duration | MIDI injections | Average | Workload peak | Setup-inclusive peak |
|---|---:|---:|---:|---:|---:|
| Two four-layer Mono notes, eight channels | 605.2 s | 504 | 35.22% | 57.1544% | 59.9229% |
| One four-layer stereo note, eight channels | 605.5 s | 505 | 25.24% | 43.4917% | 50.3442% |

Both runs maintained the expected voice counts, active streaming, zero stream
underruns and zero sampled console RX drops. They pass their workload timing
checks, but do not supersede the higher transition result above. Captures:
`logs/stereo-0-ladder-20260920-054503.log` / `.json`,
`logs/stereo-4-ladder-20260920-055612.log` / `.json`, and
`logs/capacity-layer-{mono,stereo}-full.log` for setup/cleanup boundaries.
A preceding 40-second Mono screen (`20260920-053626`) passed; the first
attempt (`20260920-053442`) stopped on a harness-only zero-based MIDI-channel
argument before any injected burst, and is excluded from acceptance.

### Simultaneous Track pressure — deadline failure

The supported 16-Track/four-layer sequencer case keeps only eight Mono render
channels active, but resolves and starts **64 layers at each simultaneous hit**.
With the same DSP/streaming/live-edit load, its 30.078 seconds of ordinary DWT
windows averaged 35.7736% and peaked at **549,652 cycles (114.5108%)**. The
setup-inclusive maximum was **566,588 cycles (118.0392%)**. The workflow retained
eight voices, active streaming and zero stream underruns/console RX drops.
Those counters therefore do not detect this callback deadline violation.
The screen was short and had no Pattern file cycle; it is failure evidence,
not a ten-minute acceptance row. Source/image identities are unchanged from
the ordinary recovery image above. Capture:
`logs/stereo-0-ladder-20260920-060830.log` / `.json` and
`logs/capacity-track-pressure-full.log`.

The separate detailed image repeated this case for 15 seconds. Its largest
setup/workload callback was 603,154 cycles, with **16 resolves and triggers,
64 voice starts**, and these disjoint costs: trigger 170,702; render 141,753;
resolve 124,266; locks 53,193; modulation 23,369; sequencer commands 22,168;
tick 17,979; controls 6,105; queue 290. Voice-start cost (135,477 cycles) is
nested in trigger cost. This shows substantial work initializing layers that
are stolen before the block renders. Avoiding initialization alone does not
remove the resolution and lock costs; a bounded admission/resolution design
must preserve whole-note, choke and sample-offset semantics. No such behavior
change was made in this validation pass. The callback gate is **blocked**, and
the guide's backend-upgrade planning condition is active while callback
features remain. Evidence:
`logs/stereo-0-ladder-20260920-061211.log` / `.json` and
`logs/capacity-pressure-detail-full.log`. Detailed percentages are diagnostic
only and are not substituted for the ordinary-image failure above.

Normal profiling-Off/detail-Off/RTT-Off firmware was restored afterward
(Daisy SHA256 `45fed7701b849639d4d911a5f13fef7ef997b2d8b6e83b3fe600087ae39d0660`).
The normal-image functional regressions and idle-state smoke check pass as
recorded at [HV-022](hardware-validation.md#hv-022--peer-restart-and-browse-delivery).
Those functional passes do not close the callback capacity failure.

## Mixed-channel sequencer/sampler validation — 2026-09-20

The unchanged scheduler-placement Daisy image (`f6c8a1957f89e08a590d989462aac553dd8cfd3a559914ba532068b8bb3c04fa`)
completed the full two-stereo/four-Mono workload at the eight-channel budget:
two oscillators, full-drive ladder, modulation, four locks per hit, sequencing,
streaming, live filter edits and ten Pattern save/load cycles. Frontend image
`78c63611b1f5425d41639b0211d5969b83eb25925655e1fc54ffe4ebd6dfbffe`
includes complete-line console replies. No audio code changed in this pass.

Capture `logs/stereo-2-ladder-20260920-031805.log` / `.json` contains 121 DWT
windows spanning 605.3 s; the workload completed in 606.44 s. Weighted callback
average is 144,912 cycles (30.2%), maximum 268,865 (56.0135%, COMFORTABLE),
with zero stream underruns and sampled console RX drops. The complete boot/setup
and workload capture (`logs/seq-sampler-mixed-full.log`) peaks at 272,900 cycles
(56.85%). This is another workload, not an optimization comparison against the
eight-Mono trial. The normal profiling-off image was restored afterward.

An earlier setup failed before playback: following a Daisy-only reflash,
the frontend browser stayed empty although the card mounted successfully.
The combined UART overflow counter increased; restarting the frontend restored
browsing. Follow-up inspection found rejected TX enqueues and an explicitly
dropped root browse response, not evidence of Daisy RX overflow. Preserve
`logs/seq-sampler-mixed-run.log`, `logs/seq-sampler-mixed-boot.log` and the failed
`logs/stereo-2-ladder-20260920-031623.json`; this is a separate peer-restart gate.
The successful retry is `logs/seq-sampler-mixed-retry.log`. Clean-commit repeat,
other channel mixes, layered/repeated-note burst timing, interrupt latency,
listening and the one-hour soak remain open.

## Sequencer placement trial — 2026-09-19

The Phase 2 callback checkpoint now has a placement-only candidate: named ITCM
sections on scheduler Start/Process/AppendRange/ComputeTriggerTick and transport
ApplyTransport/Tick. Pattern/event data, ordering and arithmetic are unchanged.
The linker places the emitted hot entry points in ITCM; some small annotated
helpers inline into their callers. Standard-library sorting helpers remain in
QSPI. ITCM use grows from 28,792 to 35,680 of 65,536 bytes; the QSPI image grows
by 40 bytes. Both images keep `-O2`, profiling On, detail Off and RTT Off.
The profiler object is byte-identical (SHA256
`9fb4c0a44933f739f13ff9a478f8d92fcb7cee32b17e793a15b3a895130d282b`).

Fresh control: source `dbd1ebf+`, Daisy SHA256
`d92696779f82dd17d855f6e2ef7e84ff6803e1b609a7b249789b376e77578609`;
frontend SHA256
`2c1f693d3a0dfdbd369b36e74cedc06ee22c4b55c0d1e9d3e574a742ba5a5403`,
using its committed native-USB console. Capture
`logs/stereo-0-ladder-20260919-044706.log` / `.json`: 605.2 seconds / 121 DWT
windows, ten Pattern file cycles, average 176,232 (36.7%) and peak 341,506
(71.1471%, REVIEW), zero stream underruns and console RX drops. Setup peaked
at 350,309 cycles (72.9810%); retained in `logs/seq-control-boot.log`.

Candidate SHA256:
`f6c8a1957f89e08a590d989462aac553dd8cfd3a559914ba532068b8bb3c04fa`.
The candidate captured 605.2 seconds / 121 DWT windows: average 168,675 cycles
(35.1406%), maximum 310,836 (64.7575%), with zero observed stream underruns or
console RX drops. Setup peaked at 316,762 (65.9921%). Nine Pattern cycles were
fully confirmed; in cycle ten the saved/loaded Pattern restarted and Back
navigation executed, but console ACK 940623 never arrived. The harness
therefore records **failed**, with its exception preserved; this is valid
callback timing evidence, not a passing complete workflow. Evidence:
`logs/stereo-0-ladder-20260919-045956.log` / `.json`, `logs/seq-itcm-run.log`,
`logs/seq-itcm-boot.log`, and the corresponding section of `logs/esp32.log`.

An unchanged-control return check flashed the original binary directly, after
verifying its SHA256 (no rebuild against the annotated headers). Its 185.1
seconds / 37 windows and three Pattern cycles passed with zero underruns or
console RX drops: mean 176,289 (36.7269%), maximum 339,485 (70.7260%), setup
353,279 (73.5998%). Evidence:
`logs/stereo-0-ladder-20260919-051348.log` / `.json` and
`logs/seq-control-repeat-boot.log`. This shorter repeat characterizes variance;
it is not another ten-minute gate row. The first 37 windows of control /
candidate / returned control averaged 176,322 / 168,604 / 176,289 cycles,
with maxima 341,506 / 305,276 / 339,485 respectively. Average cost returned to the control
level, and the control peak spread is much smaller than the candidate's gain.

Retain the placement: against the full control, average work fell 4.29% and
observed workload peak fell 30,670 cycles (63.90 µs, 6.39 percentage points).
This is a measured result for this fixture, not a worst-case execution proof.
This dirty-tree trial does not close a phase gate; clean-commit repeat, a fully
acknowledged workflow, other channel mixes, burst/latency checks and the
one-hour soak remain required.

## Callback and note-group iteration — 2026-09-18

The replacement-card eight-Mono workload first measured 336,019 cycles
(70.0040%, REVIEW). Reusing LFO configuration alone did not clear the gate:
`logs/stereo-0-ladder-20260919-030633.log` spans 610.4 s / 122 windows,
ten Pattern save/load cycles, zero underruns, average 178,111 and maximum
339,743 cycles (70.7798%, REVIEW). The image is SHA256
`ff8d8c2e5d597c2a4406027fa6d24ff88fac55d1b77b9dc8820203ba95b7024d`.
Do not claim a standalone LFO-cache performance win from this result.

Two subsequent short screens exercised whole-note admission plus an immutable
startup pitch-ratio table. They were deliberately interrupted after exceeding
the threshold; neither is a ten-minute gate result:

| Candidate | Windows | Pattern file cycles | Peak cycles | Load | Capture |
|---|---:|---:|---:|---:|---|
| Group planner in QSPI | 38 | 3 | 354709 | 73.8977% | `logs/stereo-0-ladder-20260919-031905.log` |
| Group planner in ITCM | 38 | 3 | 339174 | 70.6613% | `logs/stereo-0-ladder-20260919-032606.log` |

Both sampled zero stream underruns and dropped console bytes. QSPI candidate
SHA256: `d86a89be2a289118e72561879b9753c9bd64575ae12329909726128e04b97f27`;
ITCM candidate: `8fc82d30d72cd9d6881b01573cf21901e0f0b49a7e8c42359447a76ad115c769`.
Only planner placement changed between those two images. The earlier LFO-only
and group images differ in both admission and pitch preparation; that comparison
does not isolate either change's cost. JSON metadata retains exact image hashes,
workload state and the interrupted-run status.

Setup windows before the benchmark's steady-workload capture also exposed
higher startup peaks (359,985 / 367,132 / 352,043 cycles respectively). They are
not silently discarded as evidence: the full serial log is
`logs/callback-cache-daisy.log`, with separate boots for these images. The
follow-up placement run must inspect setup as well as the recorded workload.

The completed candidate (pitch table, whole-note admission with prospective
choke priority, planner and filter preparation in ITCM) measured **174,512
average cycles (36.4%) and 338,778 maximum (70.5787%)**, across 610.5 seconds /
122 windows and ten Pattern save/load cycles. No stream underruns or console
RX drops were observed. Setup peaked separately at 348,202 cycles (72.5421%).
The unchanged policy classifies this **REVIEW**; there is no demonstrated net
peak improvement against the pre-group baseline. Evidence:
`logs/stereo-0-ladder-20260919-033711.log` / `.json`;
Daisy SHA256 `386259b62c24a2dd338e441026be99b939b739fd3729fe1655fb830a29eb0e5c`.

### Correlated peak attribution (diagnostic image)

The separate `WAVEX_PROFILE_CALLBACK_DETAIL=ON` image, SHA256
`c65fe60a19cbb3d36d8a88b6d46bfe22d47502ddbdc01504fe0187d0a454a716`,
completed 189.1 seconds and three Pattern save/load cycles with no observed
stream underruns or console RX drops. This is diagnostic evidence, **not** a
600-second acceptance row. Capture:
`logs/stereo-0-ladder-20260919-035249.log` / `.json`; setup-inclusive boot:
`logs/callback-detail-boot.log`; correlated records:
`logs/callback-detail-peaks.json`.

Each column below describes one complete callback; unlike independent zone
maxima, the costs are known to coincide. Both blocks resolved and triggered
eight notes. The setup peak was block 92,329 / detail window 19; the captured
workload peak was block 228,235 / window 46, after a Pattern reload/restart.

| Disjoint stage | Setup peak cycles | Workload peak cycles |
|---|---:|---:|
| Note queue | 321 | 308 |
| Live controls | 5,628 | 4,897 |
| Sequencer commands/readback | 32,464 | 32,260 |
| Sequencer tick/exchange/telemetry | 42,401 | 41,890 |
| Zone resolution (8 hits) | 21,217 | 22,648 |
| Parameter locks (8 hits) | 10,790 | 10,131 |
| Group admission/voice start (8 hits) | 47,408 | 38,532 |
| Modulation | 22,475 | 19,921 |
| Rendering | 141,712 | 138,184 |
| Unmarked work / instrumentation | 27,602 | 27,599 |
| **Measured detail total** | **352,018 (733.37 µs)** | **336,370 (700.77 µs)** |

Within group triggering, `voice_start` accounted for 33,945 / 17,517 cycles
respectively. Its filter initialization cost was 16,177 / 2,592; source setup
7,583 / 6,629; envelopes 2,984 / 2,984; LFO setup 4,408 / 2,512. These are nested
costs, not extra time to add to the table. The detail total excludes peak
selection/publication and the outer profiler epilogue; instrumentation itself
perturbs timing. It does not measure interrupt-entry latency.

The next measured candidates are sequencer start/step preparation and placement:
the linked image leaves `SequencerTransport::Tick`, `SequencerScheduler::Process`
and sorting helpers in QSPI. The command path includes Pattern commit/copy on
Play; finer timing is needed before attributing all command cost to that copy.
Rendering remains the largest individual stage, but the peak combines it with
sequencer work and eight new notes. Keep `-O2`; the earlier global `-O3` trial
in this log increased average cost. Selective ITCM/inlining and then LTO deserve
controlled trials; neither an LTO gain nor a new compiler-flag gain is claimed.

### LUT candidates — 2026-09-19

The remaining repeated library powers are `ModScaleCache::Scale` (four
exponential destination mappings when their inputs change) and cutoff/pitch
parameter locks. Static note/root ratios, fades and CMSIS sine are already
lookup-based; the modulation curves themselves are quadratic/smoothstep math.
A 257-entry one-octave `2^x` table plus integer-octave scaling needs 1,028 bytes.
A float32 arithmetic simulation over 1,048,577 evenly spaced exponents from
-32 to +32 observed maximum relative error 1.02217e-6, equivalent to
0.00177 cents. This is a sampled numerical result, not an exhaustive bound,
on-device timing, audio-quality evidence or an adopted implementation.
CMSIS `arm_linear_interp_f32` provides the interpolation kernel already vendored
with libDaisy. Table initialization/placement and finite/range behavior still
need an explicit implementation and validation.

Before adding a custom table, benchmark `std::exp2(power)` against the current
`std::pow(2.f, power)`. Inspection of this ARM toolchain's hard-float libm
(`libm_a-sf_exp2.o`) shows a 32-entry `__exp2f_data` lookup, polynomial arithmetic
and hardware double-precision FMA. Its text is 208 bytes versus 840 for `powf`;
code size is not a cycle measurement. This existing implementation is the
preferred first trial under the library-first rule, with cache behavior,
numerical differences and actual DWT cost checked before adoption.

High-cutoff `TanPi` falls back to `tanf` above 12 kHz at 48 kHz; below that it
already uses a polynomial. The ladder's per-sample saturation is a rational
approximation with a divide, not a `tanhf` call. Tables for those operations
need filter response, high-resonance stability and listening tests; neither
should be bundled into the scheduler placement experiment.

## Stereo channel verification — 2026-09-16

The Daisy profiling image was built from clean tracked firmware at
`8714ad010cd297b9154233ef7e983cf2af9f0fb3`, persistent QSPI `-O2`, 480 MHz,
48 kHz and 48-sample blocks. Its SHA256 is
`8665204064c8ec5ccdf975f496a07cc49e5d47856fdbb3a20e63957308283f90`.
The reporting helper adds `+` because verification files and the frontend
console fix were being edited during evaluation; the captured Daisy audio
image was unchanged.

`scripts/bench_stereo_channels.py` drives the default eight-channel budget.
Each voice has two oscillators using the same resident stereo PCM, Oscillator
2 detuned +17 cents, the 24 dB Ladder filter at full drive, three envelopes,
two 20 Hz sine LFOs and eight modulation routes. The 120 BPM sequencer plays
all active Tracks every other step, with cutoff/resonance/pan/pitch locks.
A stereo SD audition runs concurrently; live filter edits continue and each
minute a new Pattern copy is saved and reloaded before playback restarts.
The source is `/03 Lips of Ashes.wav` (44.1 kHz, PCM16 stereo), with a resident
loop from ten to eleven seconds. Metadata JSON records exact configuration,
file cycles and sampled board state next to each log.

These are capacity measurements of the new stereo renderer, not a matched
before/after comparison against the earlier mono renderer. The two maps share
one PCM and do not reproduce the older sixteen-zone/per-pad workload. They do
not close the full phase gate or establish analog output quality.

| Voices | Duration | Average cycles | Peak cycles | Peak load | Stream underruns | Decision | Capture |
|---|---:|---:|---:|---:|---:|---|---|
| 8 Mono | 605.2 s | 176656 | 319229 | 66.5060% | 0 | STAY | `logs/stereo-0-ladder-20260916-051033.log` |
| 4 stereo | 605.2 s | 126070 | 241636 | 50.3408% | 0 | COMFORTABLE | `logs/stereo-4-ladder-20260916-054217.log` |
| 2 stereo + 4 Mono | 605.3 s | 151756 | 276542 | 57.6129% | 0 | COMFORTABLE | `logs/stereo-2-ladder-20260916-055718.log` |

All three accepted captures contain 121 profiling windows, ten successful
Pattern save/load cycles, zero stream underruns and zero sampled console
dropped bytes. The worst measured peak is 66.5060% (STAY).

The earlier four-stereo attempt,
`logs/stereo-4-ladder-20260916-053748.log`, stopped at approximately two
minutes when the harness requested Load before Save copy completed. It is
excluded from the gate results. The corrected harness waits for the new
saved filename and enabled Load softkey before continuing. The first mixed
attempt, `logs/stereo-2-ladder-20260916-055302.log`, was also excluded: its
date-stamped save name exceeded the Pattern format's 23-character limit.
Compact date/time names correct that harness error.

The final normal-firmware HIL selection passed 11/11 in 75.17 seconds:
ten stereo/Project cases plus the existing Project MIDI-routing regression.
The stereo checks use
`/Drums/Loops/loop15_3.wav.wav`, a stereo fixture small enough for WXI import.
They cover every full-budget mono/stereo mix, whole-note stealing (including
two Mono notes evicted by one stereo note), Mono
next-note lifetime and Revert, Project gain/balance/mute, per-oscillator Mono
WXI recall and manual mute edits across Solo. Digital peak meters verify the
output controls; no external audio capture or physical-panel check was made.
The frontend required a static debug reply-buffer increase from 640 to 1024
bytes: full filter values previously truncated the final `oscmono` field.
Its build and ESP32 host tests passed; the same longer values now pass HIL.

Normal QSPI `-O2` Daisy firmware was rebuilt and restored with profiling Off;
its SHA256 is
`bb741897abe28876dab82d99399df90e9afc0c87b02ef53156057938a38a3ce0`.
Build/flash logs, the final HIL transcript and binary hashes are retained
locally in `build/stereo-verification-20260916/` (gitignored). The accepted
capture logs and per-scenario metadata remain in `logs/` (gitignored).
After HIL, a backend reset cleared temporary sound/mixer settings; both
consoles responded with no loaded samples, active voices or stream, and the
frontend was left at the main menu on Track 1. The frontend retains the
tested debug reply fix. Listening, physical panel
operation, MIDI timing, a matched pre-stereo baseline and the full one-hour
phase soak remain unverified.


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
| ZDF ladder (`ladder_zdf.hpp`) | 161532 | 33.65% | 20.3% |
| Huovilainen ladder, 2x (`HuovilainenLadder<2>`) | 227922 | 47.48% | 30.9% |
| Huovilainen ladder, 4x (`HuovilainenLadder<4>`, the DaisySP model) | 335154 | 69.82% | 47.5% |

The 4x and SVF rows are at f9e823b; the ZDF and 2x rows are at a42c255 with
the same driver and the same workload. These are not gate rows
(`callback_performance.py` requires 600 s), but the deltas are the numbers
that matter: eight voices cost ~3 points on the ZDF ladder, ~17 on the 2x
Huovilainen and ~40 on the 4x. The modulation drives cutoff above Nyquist for
part of each LFO cycle, where every topology takes the exact-bypass path,
which is why the averages swing more than the peaks. Captures:
`logs/perf-svf-mod-20260914-023223.log`,
`logs/perf-zdf-mod-20260914-033817.log`,
`logs/perf-ladder2x-mod-20260914-033436.log`,
`logs/perf-ladder-mod-20260914-022750.log`.

Outcome the same day: the ZDF ladder was kept as the `Ladder` topology and
both Huovilainen variants were pruned; at working settings the three had
differed by -35 dB and the 4x model's cost could not be an eight-voice
default.

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
| 2026-09-16 | 8714ad0+ | 8 Mono voices, two oscillators, Ladder, locks, stream, file cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 176656 (36.8%) | 319229 (66.5060%) | 33.4940% | 0 | yes | STAY | Stereo channel checkpoint; unchanged Daisy image 8665204064c8; verification working tree dirty; see 2026-09-16 evidence notes |
| 2026-09-16 | 8714ad0+ | 4 stereo voices, two oscillators, Ladder, locks, stream, file cycles | 4 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 126070 (26.3%) | 241636 (50.3408%) | 49.6592% | 0 | yes | COMFORTABLE | Stereo channel checkpoint; unchanged Daisy image 8665204064c8; verification working tree dirty; see 2026-09-16 evidence notes |
| 2026-09-16 | 8714ad0+ | 2 stereo + 4 Mono voices, two oscillators, Ladder, locks, stream, file cycles | 6 | 48000/48 | 480 MHz | qspi `-O2` | 605.3s (121 windows) | 480000 | 151756 (31.6%) | 276542 (57.6129%) | 42.3871% | 0 | yes | COMFORTABLE | Stereo channel checkpoint; unchanged Daisy image 8665204064c8; verification working tree dirty; see 2026-09-16 evidence notes |
| 2026-09-18 | 3341de1d03633e4cbe648cc4ebf51eceafc85310+ | HV-019 baseline: 8 Mono voices, dual oscillators, ladder/drive, modulation, locks, sequencer, stereo streaming, Pattern saves/loads | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.2s (122 windows) | 480000 | 175783 (36.6%) | 336019 (70.0040%) | 29.9960% | 0 | yes | REVIEW | Replacement-card pre-group baseline; profile SHA256 778b0c77d07ae328733568199b41b1e69ab9960c49b76723fecacaf32b41242f; HV-019a partial, other mixes and burst tests pending. |
| 2026-09-18 | 3341de1d03633e4cbe648cc4ebf51eceafc85310+ | 8 Mono; LFO configuration cache; dual oscillators, ladder, modulation, locks, stream and file operations | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.4s (122 windows) | 480000 | 178111 (37.1%) | 339743 (70.7798%) | 29.2202% | 0 | yes | REVIEW | LFO-only candidate; SHA256 ff8d8c2e5d597c2a4406027fa6d24ff88fac55d1b77b9dc8820203ba95b7024d; 10 Pattern save/load cycles; retain REVIEW peak including full-load transition. |
| 2026-09-19 | 3341de1d03633e4cbe648cc4ebf51eceafc85310+ | 8 Mono dual-osc ladder; live note groups, pitch table, LFO cache and ITCM preparation | 8 | 48000/48 | 480 MHz | qspi `-O2` | 610.5s (122 windows) | 480000 | 174512 (36.4%) | 338778 (70.5787%) | 29.4213% | 0 | yes | REVIEW | SHA256 386259b62c24a2dd338e441026be99b939b739fd3729fe1655fb830a29eb0e5c; logs/stereo-0-ladder-20260919-033711.json; ten Pattern save/load cycles; startup separately 348202 cycles |
| 2026-09-19 | dbd1ebf5afb1dd1cbab8af857e18e97a685e1e39+ | Sequencer placement control; 8 Mono dual-osc ladder, modulation, locks, SD and Pattern cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 176232 (36.7%) | 341506 (71.1471%) | 28.8529% | 0 | yes | REVIEW | Daisy SHA256 d92696779f82dd17d855f6e2ef7e84ff6803e1b609a7b249789b376e77578609; seq-control-boot.log retains startup; no scheduler placement annotations |
| 2026-09-19 | dbd1ebf5afb1dd1cbab8af857e18e97a685e1e39+ | Sequencer ITCM; 8 Mono dual-osc ladder, modulation, locks, SD and Pattern cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 168675 (35.1%) | 310836 (64.7575%) | 35.2425% | 0 | yes | STAY | Daisy SHA256 f6c8a1957f89e08a590d989462aac553dd8cfd3a559914ba532068b8bb3c04fa; setup 316762 cycles; 9 confirmed file cycles; harness failed on missing Back ACK in cycle 10 despite logged navigation; DWT capture valid, acceptance incomplete; unchanged-control repeat reproduced higher costs; phase gate open |
| 2026-09-20 | dbd1ebf5afb1dd1cbab8af857e18e97a685e1e39+ | Two stereo plus four Mono; two oscillators, ladder, modulation, locks, sequencing, streaming, live edits and ten Pattern save/load cycles | 6 | 48000/48 | 480 MHz | qspi `-O2` | 605.3s (121 windows) | 480000 | 144912 (30.2%) | 268865 (56.0135%) | 43.9865% | 0 | yes | COMFORTABLE | Daisy f6c8a195 / ESP32 78c63611; full workflow passed, zero stream underruns. Setup/session peak 272900 cycles (56.85%). Dirty-tree evidence; other mixes, bursts, latency and one-hour soak remain open. |
| 2026-09-20 | dbd1ebf+ | 61-minute full-channel mix rotation, two oscillators, ladder, modulation, locks, streaming, live edits and 61 Pattern cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 3666.1s (733 windows) | 480000 | 149634 (31.2%) | 358030 (74.5896%) | 25.4104% | 0 | yes | REVIEW | HV-005/HV-019: zero stream underruns; final transition peak requires REVIEW; Daisy bbb29c9e, ESP32 82fbc618 |
| 2026-09-20 | dbd1ebf+ | Two four-layer Mono notes, sequencer plus repeated MIDI fan-out, two oscillators, ladder, modulation, locks, streaming, live edits, ten Pattern cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 169034 (35.2%) | 274341 (57.1544%) | 42.8456% | 0 | yes | COMFORTABLE | HV-019: 504 injected MIDI bursts; Daisy bbb29c9e, ESP32 82fbc618; no physical MIDI/listening claim |
| 2026-09-20 | dbd1ebf+ | One four-layer stereo note, sequencer plus repeated MIDI fan-out, two oscillators, ladder, modulation, locks, streaming, live edits, ten Pattern cycles | 4 | 48000/48 | 480 MHz | qspi `-O2` | 605.5s (121 windows) | 480000 | 121146 (25.2%) | 208760 (43.4917%) | 56.5083% | 0 | yes | COMFORTABLE | HV-019: 505 injected MIDI bursts; Daisy bbb29c9e, ESP32 82fbc618; no physical MIDI/listening claim |
| 2026-09-20 | dbd1ebf+ | 16 Tracks x 4 Mono layers, same-frame batch, dual osc, ladder, modulation, locks, stream and Pattern files | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.9s (121 windows) | 480000 | 169698 (35.4%) | 316812 (66.0025%) | 33.9975% | 0 | yes | STAY | ac4f254f candidate; 10 Pattern cycles, zero stream underruns; mixed-channel final-image soak pending |
| 2026-09-20 | dbd1ebf+ | Rapid full-channel stereo/Mono rotation every ten seconds, two oscillators, ladder, modulation, locks, streaming, live edits and ten Pattern cycles | 8 | 48000/48 | 480 MHz | qspi `-O2` | 605.2s (121 windows) | 480000 | 145494 (30.3%) | 287832 (59.9650%) | 40.0350% | 0 | yes | COMFORTABLE | Batched admission candidate ac4f254f, ESP32 82fbc618; rapid transitions only, final-image one-hour soak remains open |
