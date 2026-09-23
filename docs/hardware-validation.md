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
- [HV-017 — Sample Edit selection](#hv-017--sample-edit-selection)
- [HV-018 — 8-inch display bring-up](#hv-018--8-inch-display-bring-up)
- [HV-019 — Note-group allocation policy](#hv-019--note-group-allocation-policy)
- [HV-020 — Complete kit and pad-pattern workflow](#hv-020--complete-kit-and-pad-pattern-workflow)
- [HV-021 — USB console and Daisy RTT](#hv-021--usb-console-and-daisy-rtt)
- [HV-022 — Peer restart and browse delivery](#hv-022--peer-restart-and-browse-delivery)
- [HV-023 — Stream read recovery cursor](#hv-023--stream-read-recovery-cursor)
- [HV-024 — Melodic sequencing](#hv-024--melodic-sequencing)
- [HV-025 — Standalone sample saves](#hv-025--standalone-sample-saves)
- [HV-026 — Stereo snap, seams and crossfade](#hv-026--stereo-snap-seams-and-crossfade)
- [HV-027 — Sample playback channel selection](#hv-027--sample-playback-channel-selection)
- [HV-028 — MIDI expression](#hv-028--midi-expression)
- [HV-029 — Expanded modulation and live locks](#hv-029--expanded-modulation-and-live-locks)
- [HV-030 — Codec and internal-mix recording](#hv-030--codec-and-internal-mix-recording)
- [HV-031 — Arpeggiator](#hv-031--arpeggiator)
- [HV-032 — Global LFO, held locks and diagnostics](#hv-032--global-lfo-held-locks-and-diagnostics)
- [HV-033 — Instrument tags and filtering](#hv-033--instrument-tags-and-filtering)
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
| HV-001 | SD format, confirmation and recovery | Partial pass | Scratch write/readback passes at 25/12.5 MHz; recorder single-sector CRC captured at 25 MHz, selected recorder flows pass at 12.5 MHz. Recovery/soak remain open |
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
| HV-016 | Bank SD transactions and Track recall | Partial | Sparse Bank HIL passed 2026-09-18; full-Bank, DWT, MIDI timing and failure/recovery gates remain open |
| HV-017 | Sample Edit selection | Pending | Real-LVGL host checks; physical selection/render/audio unrun |
| HV-018 | 8-inch display bring-up | Partial | RGB565 restored visible UI, confirmed by user 2026-09-18; touch/brightness/load checks remain open |
| HV-019 | Note-group allocation policy | Partial | Batched 16-Track/four-layer pressure passes; final-image mixed-channel soak, held-key behavior and saved controls remain open |
| HV-020 | Complete kit and pad-pattern workflow | Partial | Full 16-pad WXI/Pattern HIL passes; listening, reboot and failure recovery remain open |
| HV-021 | USB console and Daisy RTT | Partial; RTT integrity failed | ESP32 USB console passes; Daisy USB retained pending a reliable RTT probe/readout and active-audio soak |
| HV-022 | Peer restart and browse delivery | Passed (listed cases) | Six isolated restarts, full listings, and early-selection/load regression pass; uncorrelated overlapping browse requests remain outside this check |
| HV-023 | Stream read recovery cursor | Pending | Absolute retry-position fix implemented; injected SD read failure and latency checks unrun |
| HV-024 | Melodic lanes, gates and recording | Partial pass | Lane/record/Pattern HIL and rendering checks pass; physical MIDI, audio capture and extended capacity remain open |
| HV-025 | Standalone Sample Edit Save/Save As | Partial | Short/long WAV save-copy and recall HIL, plus software-restart streaming recall pass; physical recovery, listening and capacity remain open |
| HV-026 | Stereo snap/seams and playback crossfade | Partial | Host policy/PCM/persistence checks pass; candidate callback comparison, listening and power-cycle checks remain open |
| HV-027 | Sample playback channel selection | Partial | Automated channel/control/budget checks pass; listening, write recovery and full timing/soak remain open |
| HV-028 | MIDI expression routing and reset | Partial | Host and console-injected board checks pass; physical MIDI, listening and full soak remain open |
| HV-029 | Oscillator mix/LFO-rate routes and live locks | Partial | Host and live-lock HIL pass; 85.3713% Mono callback finding remains open; listening, panel and save/reboot acceptance remain open |
| HV-030 | Codec/internal-mix recording | Partial | Two source workflows passed; later 25 MHz WAV write failed; signal, recovery and capacity remain open |
| HV-031 | Instrument arpeggiator | Partial | Host timing and edit/generated-voice HIL pass; physical timing, persistence and soak remain open |
| HV-032 | Global LFO, held locks and diagnostic counts | Partial | Four console-driven board tests pass; physical controls, listening and extended checks remain open |
| HV-033 | Instrument tags and filtering | Pending | Matched protocol-9 images; reliable card required for save/reboot |
| HV-035 | USB MIDI host and port mode | Partial | Startup/readback verified; user reports adapter working; recovery, persistence and timing acceptance remain open |

## HV-001 — SD card formatting

**Introduced:** `954bbd7`. **Gate:** [Phase 2 card storage](roadmap.md#composition-and-performance-workflows).
**Setup:** both updated images, disposable card containing recognizable test
files, second card for replacement checks, serial logs and the physical panel.
**Behavior:** [card maintenance](features/inter-mcu-protocol.md#card-maintenance).

**2026-09-18 failure report:** the user observed "Card preparation failed"
after requesting a format. `logs/daisy.log` shows an inserted 7,497 MiB card
mounting and playing a WAV before the storage-lost notification. The running
image identity and exact FatFs failure were not captured, so this does not
establish whether formatting or the following remount failed. A subsequent
read-only ST-Link read of the running SD handle (no halt/reset) found
`ErrorCode=0x00002000` (`SDMMC_ERROR_ILLEGAL_CMD`), with 15,353,856 logical
512-byte blocks matching the inserted card. This is the retained HAL error,
not proof of the first failing command. The gate remains open.
Stage diagnostics now record the `mkfs` result before remount can overwrite
the HAL error, the remount result, and any failed directory path/result. Capture
those `SD format:` lines and the tested image SHA-256 on the next explicitly
confirmed test-card attempt. Do not infer a successful erase from a failure.

**2026-09-18 authorized hardware retry:** the same 7,497 MiB card was formatted
through Settings → Storage → Format Card → Erase all data, using the real UI
touch path through the debug harness. UART remained the inter-MCU transport.
Only the Daisy was flashed; the frontend boot log identifies ESP32 app ELF
`1ede590a3...` (2026-09-18 17:50:39 build).

| Daisy binary SHA-256 | SD experiment | Observed result |
|---|---|---|
| `dcf33e4abd9bf2d491008c66517487802833e6a147f99034d5d0f6c4aa7c8e65` | Original bus configuration | `mkfs` failed immediately: FatFs 1, HAL `0x6` (data CRC + command timeout); remount then failed with HAL `0x2000`. |
| `98a101984e39c3adddfb9796bd0e925b2d3797b30a343c24d7e7a046c71cdfac` | Lower clock, original bus width | `mkfs` passed in 1,316 ms and remounted, but creating `/wavex/projects` failed with FatFs 1 / HAL `0x6`. |
| `a80f62a90625e95ccd6b6ee8d20f5027c4baac6bddb2354a0e2ec02001514b5f` | Conservative clock and narrow bus, adopted as defaults at that time | `mkfs` passed in 981 ms, remount passed, all WaveX folders were created and the UI showed success. |

Twelve consecutive named Pattern saves then passed. Loading `SD check 12`
restored Track 1 / step 1 enabled with note 73; after an ST-Link reset, the card
mounted and the same Pattern restored those values again. These test Patterns
remain on the card. Daisy reported zero underruns/dropped note events at the
checkpoints, with no samples loaded or playing. UART TX queue-full messages
were observed around storage/reboot transitions, so this is not a link-soak
pass. Evidence: `logs/sd-format-daisy-20260918.log`,
`logs/sd-format-retry{,-12mhz,-1bit}-20260918.log`,
`logs/sd-format-save-check-20260918.log`,
`logs/sd-format-reboot-check-20260918.log`, and
`logs/sd-format-success-20260918.png` (local bench artifacts).
Final pre-commit checks passed both firmware builds and all shared, ESP32 and
Daisy host suites; transcript: `logs/sd-format-precommit-20260918.log`.

**2026-09-21 clock-negotiation follow-up:** restored the configured four-bit
starting point and existing clock-only fallback at the user's request. The
inserted card reports **60,906 MiB**, unlike the 7,497 MiB card above. It mounts
at **STANDARD/25 MHz, 4-bit** in 42 ms. Loading `/03 Lips of Ashes.wav` reads
49,250,304 PCM bytes in **5.162 s / 9540 KB/s**; Load-to-ready takes **5.580 s**,
down from 33.365 s on the previous narrow-bus image. Switching to Edit takes
0.223 s.

The unique-copy Save As check **failed** with `SAMPLE_FILE_IO` (`fileerror=6`)
at 1% progress. The test was interrupted after observing terminal failure,
rather than waiting its 600-second success timeout. This is not a write pass:
mount negotiation exercises reads and does not automatically retry failed
file jobs at a lower clock. The 12.5 MHz/four-bit write comparison and forced
clock-fallback checks remain unrun; roadmap work resumed at the user's request.
No format was attempted, and no one-bit fallback was restored.

Both firmware builds and all 1,669 host tests pass. Flashed Daisy SHA-256:
`bc3222812411df9d93c843be5b30927e3b5519d8f92d8b8ac9bb8bf5f7cd2c96`;
unchanged ESP32 SHA-256:
`a4dbb2b269ad490660609799098dc9e2935ff5a50bf06e68696ebb66b78b644b`.
Evidence: `logs/sd-auto-clock-{checks,flash,daisy,load,load-transcript,save}.log`
and `logs/hil-20260921-042630.log`. Recheck using the same timed Load and
`test_standalone_save_copy_then_save_survives_pool_reload` with
`--hil-sample2 "/03 Lips of Ashes.wav"`; require successful unique-copy Save As,
sidecar Save and unloaded-pool reload before calling writes validated.

### First-error write investigation, 2026-09-21

**Setup:** normal Stage A, four-bit SDMMC, the same attached 60906 MiB card,
profiling disabled, ESP32 SHA256
`af3073b1f3b0202962c1b131f737a9ce25d3fb84f57a5565a7e6c4a6b4076510`.
Daisy diagnostic images over `bd60138`:

- 25 MHz SHA256 `856b44bda8ed08667ad6485574afbeec2d7bc3f9febb89f313aa4510f165dac1`.
- 12.5 MHz SHA256 `d5d6ba62ee88769f0122d78dc57ffff5485df023769e45232da5a6628326583a`.
- Final normal 25 MHz SHA256 `c11b23470a15537b0ef05808dc619f9994777ab4901d7146043f727c10577e23`
  (avoids unnecessary foreground snapshot copies/masking and includes IDMA error flags).

Run [the SD write probe](features/debug-harness-and-hil.md#sd-write-diagnosis)
with an otherwise idle storage session. Each case exclusively creates a scratch
file, writes deterministic data, syncs/closes/reopens, verifies every byte, then
removes only that successful scratch file. Preserve first failure evidence and
boot fresh between clock configurations; recovery is a separate experiment.
Require every case to complete with no retained disk/IRQ failure or byte mismatch.

At 25 MHz, 512 B, 4 KiB, 1 MiB and 48 MiB files passed using 4 KiB chunks.
The 48 MiB complete write/readback/delete cycle took 34.810 s (not a raw write
throughput measurement). Separate 1 MiB cases passed with 512-byte chunks,
a buffer shifted four bytes from cache-line alignment, and a 44-byte first
write followed by 4 KiB chunks. All sampled underrun counts were zero.

The same idle matrix passed at 12.5 MHz; its 48 MiB cycle took 43.811 s.
At 12.5 MHz with one resident stereo voice playing, 1 MiB and 48 MiB probes
with a 44-byte prefix also passed (48 MiB: 57.759 s), with zero sampled
underruns. Both real codec/internal recording capture/audition/Save/assignment
HIL workflows then passed (2 tests, 6.85 s), and `SDIO` remained clear.
Evidence: `logs/sd-write-125-{aligned,single,shift,prefix,playback}.jsonl` and
`logs/sd-diagnostics-recording-125.log`. These are short selected passes;
they do not establish that 12.5 MHz always works or that remount recovery works.

After the initial 25 MHz probe matrix, both codec/internal recorder HIL
workflows failed Save.
The retained first error was **one sector**, LBA 582848, buffer `0x240302ac`
(AXI SRAM, word aligned), write DMA active. Before HAL handled the IRQ,
`STA=0x00000002` indicated data CRC failure, `DCOUNT=0`, `CLKCR=0x00004004`.
The post-handler HAL error was `0x6`: data CRC (`0x2`) plus command-response
timeout (`0x4`). The pinned HAL error branch issues STOP_TRANSMISSION and ORs
its result into ErrorCode, so the timeout is secondary to the observed CRC flag.
The recorder reported `phase=3`, `fatfs=1`, `bytes=0`: first payload write,
not header generation. This establishes a device-I/O failure in the recorder
path, without proving card, wiring, power or driver as the physical cause.

The final normal 25 MHz image also passed 1 MiB and 48 MiB write/readback
with a 44-byte prefix during one resident stereo voice (48 MiB: 49.488 s,
zero sampled underruns). The follow-up recorder HIL passed codec Save but
failed internal-mix Save: another **single-sector** CRC, LBA 472896,
DMA buffer `0x24076a98`, `STA=0x2`, HAL `0x6`, this time in header phase 2.
Thus short successes at 25 MHz coexist with reproduced failures; neither
file size nor simultaneous resident playback alone explains the fault.
Evidence: `logs/sd-write-25-playback.jsonl` and
`logs/sd-diagnostics-recording-25-final.log` (1 passed, 1 failed).
A further named internal take failed Save and explicit same-clock retry with
`REC_IO`; its 21744 captured frames remained available until deliberate Discard.
The original `SDIO` snapshot stayed byte-for-byte identical through both later
failures. Evidence: `logs/sd-diagnostics-retry-named.log`. No automatic clock
fallback/remount was exercised by this recorder retry; recovery remains open.
The bench ended on the final normal 25 MHz image, idle, with no resident samples
or active voices and the first-failure record retained for inspection.

Evidence: `logs/sd-write-25-{aligned,single,shift,prefix}.jsonl`,
`logs/sd-diagnostics-recording-25.log` and `SDIO_FAIL`/`SDIO_REG` in
`logs/daisy.log`. First-failure retention/reset has three host tests; 866 Daisy
host tests and both Stage A/Stage B builds pass. This investigation does not close write-soak, loaded-audio,
recovery, power-loss or phase acceptance.

### Card/driver isolation, 2026-09-21

**Setup:** same card, socket and power arrangement; four-bit 25 MHz, normal
Stage A and the unchanged ESP32 image identified above. Software reset/remount
separates experiments; no card swap, rewiring or socket-rail measurement was
available. Card CID words are `03534453 4b363447 8061e321 c2016908`.
`DBGMCU_IDCODE=0x20036450` identifies revision V (`REV_ID=0x2003`).
[ST ES0392 Rev 15](https://www.st.com/resource/en/errata_sheet/es0392-stm32h742xig-stm32h743xig-stm32h750xb-stm32h753xi-device-errata-stmicroelectronics.pdf)
Table 2 and §2.8 do not identify a matching revision-V single-block write-CRC
limitation; this does not exclude an unlisted silicon or software fault.

Test images over `cc60688`:

- Pattern/directory image: `b06b821fdbb7b8e116f96dde81af253215fe5031ab984ac06c6946f4ef138dce`.
- Aligned-buffer/flow-control image: `e8aecc41d59784d98990f194d94ed3553a36a779c60b15e32c4647e43ab4d2d9`.
- Transfer-spacing image: `d284b38275f7d48851519b632c5dc04dc6b7bb076cf9a949e7beaea8f4d7d8b0`.

| Controlled comparison | Result | What it establishes |
|---|---|---|
| 1 MiB, 44-byte prefix, hash/zero/FF/AA55, root and recordings directory | All eight exact readbacks passed | These payloads/directory choices alone do not reproduce the recorder fault |
| Recorder, normal direct DMA | Both sources failed Save | Failure remains reproducible after passing scratch probes |
| Dedicated aligned AXI buffer, same sector counts and clock | Initial two recorder tests passed; next two failed | Alignment/cache-line isolation alone is not a fix |
| SDMMC FIFO hardware flow control, direct buffers | Four recorder tests passed; next two failed | Flow control alone is not a fix |
| 100 us idle before each disk transfer, direct buffers | Both recorder tests failed | Simple additional inter-transfer spacing is not a fix |

The aligned-buffer failure retained caller address `0x240302cc` and actual
IDMA address `0x24076cc0`: the failing **single-sector** write really used the
32-byte-aligned staging buffer. `STA=0x2`, HAL error `0x6`, clock `0x4004`.
The flow-control failure also retained a single-sector data CRC, with
`CLKCR=0x24004`, confirming hardware flow control was enabled. Neither captured
failure set the FIFO underrun or IDMA transfer-error status bits. This narrows
those specific hypotheses; it does not exonerate every driver path.
The spacing experiment failed a seven-sector write (LBA 476225), with
`STA=0x11002`, HAL `0x2`, `DCOUNT=3072`: CRC failed with six sectors still
remaining. The problem therefore also affects multi-block writes; it is not
specific to CMD24/single-sector handling.

Evidence: `logs/sd-isolate-d{0,1}-p{0,1,2,3}.jsonl`,
`logs/sd-isolate-recorder-{direct,bounce,bounce-repeat1,flow,flow-repeat1,flow-repeat2}.log`,
`logs/sd-isolate-{bounce,flow,gap}-info.log`,
`logs/sd-isolate-recorder-gap0.log`, and the retained `SDIO` records in
`logs/daisy.log`. USB re-enumeration skips were retried after a verified boot;
skips are not passes.

The final transfer-spacing image above was reflashed with boot defaults restored:
`SDIO INFO` confirmed mode 0 and `CLKCR=0x4004`, with no retained error and no
active voices, samples or streaming. The four new pattern/readback and mode-busy
guard HIL cases passed (6.53 s, `logs/sd-isolate-probe-hil.log`). These diagnostic
checks do not turn the failed recorder comparisons into acceptance. The normal
Stage A, Stage B and release builds passed, as did all 866 Daisy host tests;
the release ELF contains no probe/experiment symbols.

- [ ] **001g — Card versus assembly:** with the same image, 25 MHz/four-bit,
  and mode 0, run repeated codec/internal Save cycles on this card and a second
  known-good FAT32 card. Record CID, first-error snapshots and pass/fail order;
  alternate cards to avoid confusing elapsed time with card identity. Separately
  write/sync/readback a scratch file on the original card through a known-good
  PC reader. A second card passing suggests card-specific compatibility/timing;
  it does not by itself prove defective flash.
- [ ] **001h — Socket power:** scope VDD at the card socket relative to its
  nearby ground during a failing Save. Retain min voltage and droop/ringing
  traces, then compare a verified stable supply/local decoupling arrangement.
  ST-Link target-voltage telemetry is not a socket transient measurement.
- [ ] **001i — Signal integrity:** check continuity, actual external pull-ups,
  grounding, socket/adapter construction and any level shifters against the
  component specifications. Compare the same workload with a short, direct
  harness; scope clock/command/data at the socket for edge quality and timing.
  Keep four-bit mode. The current vendor setup changes GPIO slew along with
  clock speed, so a 25-versus-12.5 MHz pass/fail comparison is not a pure
  clock-only experiment. Keep wiring/supply changes separate and retain traces.
- [ ] **001j — Remaining driver isolation:** if both cards fail on a verified
  electrical setup, compare identical files/transaction ordering against an
  upstream minimal DMA/FatFs application, preserving clock, GPIO configuration
  and buffer placement. Do not promote a short software workaround to production
  without repeated recorder, byte-readback and loaded-audio acceptance.

### Replacement-card write probe, 2026-09-23

User confirmed the replacement card was installed before this run. The existing
running image reported four-bit 25 MHz (`CLKCR=0x4004`), experiment mode 0,
silicon ID `0x20036450`, and CID `1b534d30 30303030 106ca815 5a00d784`.
The serial log confirms removal/unmount followed by insertion and successful
remount of a 7497 MiB card (15,353,856 sectors) at 25 MHz/four-bit.
Neither board was reflashed or reset for this run; running image hashes were
not independently verified, so this is not a controlled matched-image card A/B.
The initial retained-error latch was clear; voices, resident samples, streaming
and sampled underruns were all zero.

`scripts/bench_sd_write.py` stopped at the first default case: a 512-byte file,
4 KiB application chunk, no prefix/shift/gap. Exclusive creation succeeded,
then the write failed at offset 0 with FatFs `FR_DISK_ERR` (`1`), after 2 ms
for the probe. The failed scratch file `/wx338aaa.tmp` was retained. The
4 KiB, 1 MiB and 48 MiB cases were not run; sync/readback was not reached.

First failure: write, LBA 3314, one sector, aligned AXI buffer/IDMA base
`0x240057e0`, IRQ captured, pre-handler `STA=0x1008`, `DCOUNT=0`,
post-handler HAL error `0x0c` (data timeout plus command-response timeout).
The primary captured error is data timeout, unlike the earlier card's CRC
failure; this is not evidence of an absent SD interrupt or a 30-second software
completion wait. Sampled underruns remained zero. The failure latch was left
intact and no recovery, retry, formatting or clock change was attempted.

Evidence: `logs/sd-card-swap-20260923-write.jsonl` and corresponding
`SDIO_FAIL seq=1248` / `SDIO_REG` records in `logs/daisy.log`.
HV-001g remains partial: repeat with verified image identities/fresh mount and
alternate cards before attributing the difference to card identity alone.

**Remaining:** isolate card/socket/wiring integrity and the reproduced wider-bus
write failure; successful reads alone do not prove write stability. Measure
sustained WAV streaming and write soak with negotiated clocks. Cancellation,
replacement, interruption, active-playback formatting and WAV/Instrument
reboot recovery remain unrun; the full HV-001/phase gate stays open.

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
  Retain the `SD format:` serial lines; on failure record the first failing
  stage, FatFs result and HAL error before attempting recovery.
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

- [ ] **Resident loading liveness (2026-09-22):** On matched images, loop a
  Pattern and hold a gated keyboard note while loading a large admitted WAV.
  Release/repress the note, move controls and send MIDI clock throughout the
  load; verify bounded delivery, continued heartbeat and no RX overflow or
  stuck notes. Attempt another load, recording, save and audition while busy:
  they must leave the active load intact. Remove the card during payload I/O,
  verify no partial Pool entry/allocation leak, then reinsert and explicitly
  retry. Capture maximum foreground pass duration, UART overflow counts and
  callback DWT peaks under the same workload before/after. Host job tests pass
  separately; no device latency, SD recovery or capacity acceptance is implied.

- [ ] **Probability regression (2026-09-22):** Capture repeated 25/50/75%
  steps over a long run and compare hit rates to their settings. Verify seeded
  restart repeatability and collect matched before/after DWT peaks for the
  probability workload. Host distribution tests do not close device timing.

- [ ] **Retrigger edit regression (2026-09-22):** Loop a retriggered drum step
  while editing another Track. Capture both repeats at their original offsets;
  then mute the source row, switch it melodic, and move its next boundary
  earlier. Verify cancellation/clipping, no late repeats after Pattern launch,
  and matched before/after callback DWT peaks. This check is unrun.

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

**2026-09-20 — 005b partial / REVIEW:** The full channel-budget workload ran
for 3,667.23 seconds with all five stereo/Mono mixes, live transitions and 61
Pattern save/load cycles. Stream underruns and sampled console RX drops stayed
zero. The callback averaged 31.17%, but the final mix-change/file-cycle window
peaked at 74.5896%, so the capacity acceptance requirement remains open.
See [the exact workloads, images and captures](callback-performance-log.md#full-channel-soak-and-transition-peak--2026-09-20).
This is digital state/DWT evidence; physical MIDI, panel and listening checks
are not covered, and console RX drops are not a musical-note queue counter.

The complete Phase 2 gate also depends on unfinished MIDI clock, panel and
session-persistence work listed in the roadmap. Passing these checks alone
does not close it; add their runnable entries as those implementations arrive.

## HV-006 — Mixer controls and master

- [ ] **Track pan regression (2026-09-22):** Hold a keyboard note, move Play pan,
  then trigger more notes and a sequenced step. Held/new notes must agree;
  another Track and sample preview must stay unchanged, and a step pan lock
  must override the Track adjustment. Include noncentral zone/Instrument pan
  and capture matched before/after DWT peaks. Physical/listening and timing
  results are not yet recorded.

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
- [ ] Restore saved Bank selections and inject dependency failures using
  **HV-016j** below. Project success must publish the matching Bank index.

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

**Status:** Partial. Sparse Bank HIL passed on 2026-09-18; the full
acceptance cases below remain open. See the dated evidence after the checklist.
**Design / gate:** [Bank persistence](features/bank-persistence.md),
[Phase 2.5](roadmap.md#phase-25--sampler-instrument-layer).
**Setup:** Both integrated images, a backed-up disposable card, a sparse Bank
with occupied first/last slots and two-oscillator Instruments, serial capture
and DWT/audio monitoring. Open Project → Shift → Banks. Use new names;
preserve the source Bank. Capture both image identities before starting.

- [ ] **016a — Sparse copies:** Create an empty Bank, store in slots 1/128,
  copy it while replacing one slot, then clear one slot into another copy.
  Reload each file. **Pass:** Stable slot identities, names/tags and both
  oscillator/LFO settings; the original Bank and its embedded bytes are unchanged.
- [ ] **016b — Failure isolation:** Try duplicate names, an occupied temporary
  name, insufficient free space and removed media.
  **Pass:** Rejected saves publish no destination, earlier files remain usable,
  and only the job's own temporary is eligible for cleanup. Failed reads never
  replace the live Bank or Track. If publication succeeds but index reload fails,
  the new file may exist; reopen it explicitly and verify the old active Bank was
  retained. In-flight cancellation remains adapter-test coverage only: there is
  no device cancellation entry point.
- [ ] **016c — Service budget and recovery:** Copy a full Bank while resident
  playback runs; measure foreground pump worst case (including one new WXI
  encode/read), callback DWT and underruns. Reboot and reload the successful copy.
  **Pass:** No underruns, acceptable control responsiveness under the performance
  policy, and the saved Bank reopens with the same slot contents. Record image
  hashes, card identity, timings and evidence before closing this entry.
- [ ] **016d — Track recall and confirmation:** Load two Tracks sharing samples,
  give them different MIDI/mix settings, and store a kit and keyboard Instrument.
  Cancel Recall; change Track while confirmation is open; then confirm Recall
  into one Track. Edit it and recall again. **Pass:** Cancellation is inert;
  confirmed recall restores the stored sound only on its captured target, keeps
  Track MIDI/mix settings and the other Track's sound/sample ownership, and
  preserves the Bank file. Repeat with newly loaded sample dependencies.
- [ ] **016e — Recall failure and link recovery:** Remove a required sample or
  fill sample memory before Recall. Disconnect the frontend during a Bank job,
  reconnect and inspect status/files before retrying. **Pass:** Failed recall
  retains the old Instrument, other Tracks remain playable, and reconnect does
  not repeat Store/Clear/Recall. Record memory before/after failures.
- [ ] **016f — Playback during recall:** Run resident sequencing with MIDI clock
  input/output while opening/storing/recalling Banks. Measure foreground service
  time, callback DWT, clock jitter and underruns. **Pass:** No underruns or lost
  transport state; non-target Tracks remain continuous, the target changes only
  after successful staging, and response/timing meet the performance policy.
  Include Program Change recall; its additional cases are 016h below.
- [ ] **016g — Bank sample preload:** Create a Bank containing shared and unique
  dependencies across both oscillator maps. Unload one dependency, keep another
  Track sounding, select an empty slot and press Preload. Repeat; switch Banks;
  explicitly unload unneeded samples. Repeat with a late missing file, full
  sample memory and removed media. **Pass:** Success pins every dependency once,
  leaves all Track sound/routing/mix/editor state unchanged and stops no Track;
  failures restore the entire original Pool and release all new PCM/pin changes.
  Bank changes do not silently unpin samples. Repeat with all 128 slots and
  maximum maps; collect service timing, callback DWT, audio continuity and
  underruns. No in-flight UI cancellation is available.

- [ ] **016h — MIDI Program Change recall:** With both current images, route
  one Track to a MIDI channel, a second to Omni, a third to the same channel
  with Program disabled, and a fourth to Off. Use Project → Shift → Program
  On/Off; save/reload a Project to check retention. Open a Bank with distinct
  programs at raw 0/127. Send Program Changes from DIN and USB while the
  non-target Track sounds and sequencing/clock runs. Repeat the same program;
  try an empty slot, missing dependencies, exhausted RAM, bursts during busy
  storage, and MIDI disconnect/reconnect. **Pass:** Only enabled matching/Omni
  Tracks receive private Instrument copies, routing/mix remain unchanged, and
  failure preserves all target Instruments/Pool ownership. Busy events never
  replay later or against a different Bank. No confirmation dialog is required
  for MIDI; UI Recall still requires one. Record source/target identity, service
  time, callback DWT, physical MIDI timing, audible continuity and underruns;
  repeat with all 16 targets and the largest Instrument. Full-load/electrical
  tests and Project-setting persistence remain unrun for this entry.

- [ ] **016i — Slot copy/move:** Use a Bank with distinct Instruments in slots
  1/128 and empty slot 2. Shift → Slot tools marks an occupied source; choose
  another destination and a new Bank name. Cancel and confirm Copy/Move into
  both empty and occupied slots, in both directions. Reopen the original and
  each new Bank; recall the transferred sound. Repeat with stale/disconnected
  status, duplicate names, nearly-full/removed media and a full Bank while
  resident sequencing/clock runs. Reboot and reopen successful copies.
  **Pass:** Source/destination and replacement/source-clear warnings are clear;
  identical slots/empty sources cannot transfer. Original files and embedded
  Instrument settings/extensions remain intact. Copy keeps both slots, Move
  clears only its source in the new Bank. Tracks/Pool are untouched, failed
  writes retain the active Bank, reconnect never replays, and saves check space.
  Record foreground timing, callback DWT, audible continuity and underruns.
  Sparse automated UI/UART/SD checks do not close full-media/reboot/timing cases.

- [ ] **016j — Project Bank restoration:** Save a Project with a populated
  Bank selected; switch Banks and save a second Project. Load each and inspect
  slots 1/128, then recall a sound or send an enabled Program Change. Repeat
  after reboot. Load a Project with no Bank reference and confirm New; cancel
  both operations before confirming. Remove/corrupt the referenced Bank, fail
  a later Track dependency and exhaust staging RAM before another Load.
  **Pass:** Saved file identity/index restore only with successful Project commit;
  New/empty references clear selection. Failure preserves the old Bank revision,
  Tracks and Pool; stale confirmations cannot act on the replacement Bank.
  Missing Bank-only WAVs do not prevent Project Load or admit/pin extra samples;
  subsequent explicit Preload/recall fails without partial publication. Existing
  user pins survive. Record image identities, RAM, DWT/underruns and evidence.
  `tests/hil/test_project_bank.py` covers empty-Bank selection/clearing through
  both boards without WAV prerequisites; populated/recovery/timing checks remain
  separate acceptance requirements.

**2026-09-18 Project Bank restoration result (016j, partial):**
`tests/hil/test_project_bank.py` passed on both boards (1 test, 53.46 s).
Two empty Banks were saved with separate Projects; each Load restored its
selected filename. Saving after switching Banks captured the new selection.
New and loading a Project saved without a Bank cleared the selection. After a
Daisy reset through ST-Link, explicit loads of all three files restored the same
two selections/empty state. The final idle readback reported zero voices,
samples, streaming and underruns; this is not an audio workload or soak result.

- Daisy binary SHA-256:
  `63289d90ce4ffa8f8f6ca4c8f28fc4e46b0a366847047b0591bcbdd7678ea143`.
  Includes the HV-001 SD fallback; the original attached 8 GB card was used.
- ESP32 unchanged from HV-001: boot ELF prefix `1ede590a3`, build Sep 18
  2026 17:50:39. Only Daisy was rebooted for the restoration check.
- Saved Projects `HILPB 5572396975797A`, `HILPB 5572396975797B`, and
  `HILPB 5572396975797C` remain on the card with their two empty Bank files.
  Earlier uniquely named trial files are also preserved.
- Evidence: `logs/project-bank-hil.xml`, `logs/project-bank-reboot.log`,
  `logs/project-bank-daisy.log`, `logs/project-bank-esp32.log`,
  `logs/project-bank-flash.log`, and `logs/project-bank-precommit.log`.
- All 70 loader/session host tests and the Daisy build/test hooks passed.
  Tests include populated slot 128, externally renamed Bank identity, missing
  Bank-only WAVs without implicit preload, stale revision rejection, missing or
  malformed Banks, unsupported paths, and rollback after a later Track failure.
- Populated-Bank/audio/Program Change restoration, physical dependency/memory
  failure injection, full power cycling and DWT checks remain open. UART TX
  queue overflows were logged during the Project operations despite successful
  UI readback; this run does not establish lossless transport or timing bounds.

**2026-09-18 slot copy/move validation:** Host wire, storage, session, dispatch
and real-LVGL checks pass, including unknown embedded WXI bytes and failed
publication. Both firmware images compile. The extended two-board HIL case is
implemented but **unrun**: `make flash-all` could find neither the ESP32 UART
nor Daisy CDC device in the privileged devcontainer (`logs/slot-flash.log`).
No new images were flashed and no slot-transfer hardware result is claimed.
016i, physical rendering/profile checks, and the broader HV-016 gate stay open.

**2026-09-18 bench result:** `tests/hil/test_bank_files.py` passed (1 test,
32.93 s), covering subsets of 016a/b/d. A sparse Bank with slot 128 populated
survived Store/Save/Clear copies and reopening the original. Cancelled recall
was inert; confirmed recall restored LFO settings and preserved target MIDI/mix.
Both shared-sample and newly admitted PCM recall retained the other Track's
voice. Duplicate destinations and missing Bank files preserved the active Bank.
Reported underruns stayed at zero. This is digital/state evidence; no analog
capture, callback DWT or MIDI timing was collected.

- Daisy persistent QSPI image: SHA-256
  `1ded47ac2cfc9b5ae6c83d1ff11c943a200a28c82fd9a8c6661a8f9547c9d1c9`,
  debug harness on, callback profiling off, normal `-O2` build with storage
  sources at their existing `-Os` settings.
- ESP32 image: SHA-256
  `91d8efad86c6f4cc9937b7bcbf8d4f1a272a6b02b165e9a76b765cb04f412880`.
- Source base: `b540e8d` plus the Bank timing/HIL change. Card: attached bench
  card, SDIO STANDARD/25 MHz; manufacturer/model/capacity not captured, so this
  is not a qualified card-performance result. Fixtures were
  `/Drums/Kicks/bassdr01.wav` and `bassdr02.wav` in the same directory.
- Saved copies: `HILB 733797120592628A.wxb` through `...D.wxb` under
  `/wavex/banks`. These remain available for inspection.
- Evidence (local, gitignored): `logs/bank-hil.xml`,
  `logs/hil-20260918-121631.log`, `logs/bank-hil-result.log`.

| Operation | Pumps | Maximum foreground step | Summed foreground work |
|---|---:|---:|---:|
| New | 137 | 7,396 µs | 21,729 µs |
| Store copy | 150 | 9,011 µs | 27,965 µs |
| Recall, resident sample | 21 | 6,161 µs | 22,460 µs |
| Save copy | 159 | 7,904 µs | 27,650 µs |
| Clear copy | 147 | 7,914 µs | 25,222 µs |
| Open | 11 | 1,933 µs | 2,746 µs |
| Recall, sample unloaded | 24 | 5,988 µs | 29,287 µs |

An earlier partial run observed a **74,021 µs New step** before a HIL Shift
navigation error stopped that run. Preserve that outlier: evidence is
`logs/hil-20260918-121339.log`, on Daisy image
`dba23aa7702a2c340f175b1205a275bc90cb624069d0a2627d43b05485f8fe7a`
and the same ESP32 image. Later results do not erase SD latency variation.
Full 128-slot/worst-zone-count fixtures, card identification, callback DWT,
clock jitter, nearly-full/removed media, missing-dependency/memory failures,
reconnect, reboot and power interruption are still required. No full HV-016
case or phase gate is marked passed by this sparse regression.

**2026-09-18 preload bench result:** The extended Bank HIL passed (1 test,
40.70 s), repeating the earlier functional regression and a subset of 016g.
The Bank had slots 1/128 occupied with one-zone Instruments and distinct WAVs.
One sample was unloaded; the other was shared by two Tracks. Preload from empty
slot 2 admitted the missing PCM, preserved both Track states and the target's
editor revision, and retained another Track's held voice. A second preload
reused the same resident IDs. Reported underruns remained zero. This does not
establish audible continuity, pin-release behavior or failure recovery on media.

- Source base `28f1340` plus the preload change, before commit formatting.
- Daisy QSPI image SHA-256:
  `2c05b60626f9da7e0588d000675b658518e205853dd331c69e228254a6f11dbf`;
  debug harness on, callback profiling off, normal `-O2`/storage `-Os` settings.
- ESP32 image SHA-256:
  `bb535917da1dd5059b5274269f5b8299f14e72f4b9bec876d43abb3f10362c92`.
- Same attached card and two WAV paths as above; card identity remains unknown.
  Saved copies `HILB 735230596373441A.wxb` through `...E.wxb` remain on card.
- Evidence (local, gitignored): `logs/preload-hil.xml`,
  `logs/preload-hil-result.log`, `logs/hil-20260918-124024.log`.

| Preload workload | Pumps | Maximum foreground step | Summed foreground work |
|---|---:|---:|---:|
| One cold and one shared dependency | 57 | 10,846 µs | 34,357 µs |
| Both dependencies resident | 54 | 10,700 µs | 27,041 µs |

Full-bank/responsiveness profiling, hardware failure injection, callback DWT,
MIDI jitter and soak remain open. The earlier 74 ms SD outlier remains relevant;
these sparse measurements do not bound latency or close any whole checklist item.

**2026-09-18 Program Change bench result:** Extended Bank HIL passed (1 test,
52.11 s). Frontend `MIDIPROGRAM` injected into the production parser/forwarder,
then traversed the real UART and Bank SD/recall path. Raw program 127 restored
stored LFO settings on one channel-matched and one Omni Track. A same-channel
Track with Program disabled through the Shift softkey retained its editor
revision and held voice. Empty program 1 preserved all three revisions; repeat
program 127 triggered a new recall. Track MIDI/mix and opt-out readback remained
correct; reported underruns stayed zero. This is a subset of 016h, not a
DIN/USB cable or acoustic-continuity result.

- Source base `37d04b5` plus Program Change changes, before final Bank-page
  outcome-message text and commit formatting.
- Daisy QSPI SHA-256:
  `c0343694e2aafe139256ad4dc9019675031c6f322c3b57da6a8777e7b84da055`;
  debug harness enabled, callback profiling disabled, normal `-O2`/storage `-Os`.
- ESP32 SHA-256:
  `7dad5bc9be5d0409b01b09834fd7e7a301fd6132f251a8a38933992a962f4636`.
- Same attached card/WAV fixtures as prior runs; card identity remains unknown.
  Saved copies `HILB 736410400100332A.wxb` through `...E.wxb` remain on card.
- Evidence (local, gitignored): `logs/program-hil.xml`,
  `logs/program-hil-result.log`, `logs/hil-20260918-130005.log`.
- Two-target resident recalls: 27 foreground pumps each; maxima **5,902 µs** and
  **5,781 µs**, summed work **22,946 µs** and **22,650 µs** respectively.
  These sparse service measurements do not establish callback DWT, wire jitter,
  end-to-end note readiness or a full-Bank latency bound. No whole gate closes.

## HV-017 — Sample Edit selection

**Status:** Pending. Real-LVGL host tests cover selection, cancellation,
removed/offline samples and stale page rejection; physical behavior is unrun.
**Design / gate:** [Sample Edit](ui-architecture.md#page-contract-and-lifetime),
[Phase 1.5](roadmap.md#phase-15--sample-editing).
**Setup:** Both images, at least ten resident samples with different lengths,
stereo/mono files, two assigned Tracks, serial/audio capture and render profiling.

- [ ] **017a — Select and cancel:** In Sample Edit, Shift → Select; touch a card,
  Select, then repeat using encoder/Previous/Next across pages. Cancel a different
  choice. **Pass:** The correct name, waveform, duration and saved markers appear;
  Cancel retains the old sample; Track assignments and current Track are unchanged.
- [ ] **017b — Lifetime:** Audition and drag a marker, immediately open the picker
  and select another sample; repeat while waveform data arrives. Remove a focused
  sample or disconnect the backend before selecting. **Pass:** Audition stops,
  the pending edit belongs only to the old sample, invalid selection stays in
  the picker, and no stale waveform or marker request affects the new sample.
- [ ] **017c — Panel/render:** Exercise touch, encoder, exit/re-entry and empty
  Pool; profile idle polling and selection during resident playback. **Pass:**
  Readable layout, no idle full-screen repaint, no stuck controls or underruns.

**Blockers:** Requires the new frontend image and physical touch/audio checks.
No hardware result is claimed by the host tests.

## HV-018 — 8-inch display bring-up

**Introduced:** 8-DSI-TOUCH-A display migration.
**Design / gate:** [Display bring-up](roadmap.md#8-inch-display-bring-up),
[display constraints](ui-design-constraints.md#display-bring-up-and-remaining-limits).
**Setup:** ESP32-P4 with the attached 8-DSI-TOUCH-A panel, USB flash and
console connections; paired Daisy for playback/load checks.

- [x] **018a — Flash and boot:** Build and flash the ESP32 debug image using
  [flashing.md](flashing.md). Capture the console and a UI screenshot.
  **Pass:** Flash verification succeeds, panel/touch initialization succeeds,
  the UI starts without a panic, and the screenshot is 1280×800.
- [ ] **018b — Physical image and touch:** Inspect orientation, colours and
  all four edges; tap controls near each corner and the centre. Exercise
  Shift plus softkeys, two held pads/dials and independent releases, then
  five contacts. **Pass:** Correct landscape image and coordinates, readable
  unclipped controls and no stuck contacts. Ten-contact support is deferred.
- [ ] **018c — Brightness and wake:** Change Settings → Display brightness;
  leave the panel idle until blanking, then wake by touch and encoder.
  **Pass:** Brightness visibly changes, blanking works and wake restores
  the chosen level without losing input.
- [ ] **018d — Rendering under load:** Navigate and scroll during sample
  loading/playback, inspect for corruption/tearing, and record render timing,
  heap/stack headroom and audio underruns. **Pass:** No corruption, crash or
  audio underruns; record any tearing separately. Partial flushes do not
  establish tear-free scanout or close the full phase gate.

- [ ] **018e — Menu first use and Locks redraw:** On matched images, measure
  Sample Edit entry and the first visit to every parameter window, Play entry
  and first/repeated Keys switches, and Record entry and name-keyboard open/close.
  Repeat while notes/streaming run; capture RENDER/sysmon, free heap and both
  image identities. All values must initialize correctly; no clipped controls,
  lost edits, stranded notes, heap growth across page cycles or audio underruns.
  Enter/leave held-step Locks with transport stopped and running: require partial
  redraws, measure against the historical 243.52 ms full refresh and record any
  remaining frame-budget overruns. Host-only invalidation and lifecycle tests
  pass; device measurements are **unrun** (flash approval outstanding).

**Initial run:** 2026-09-18, ESP32 debug build from `f63e590` (display code
unchanged by the subsequent documentation reconciliation in `1d5834b`),
firmware version 0.5.0. App SHA-256:
`25882810257a453cf78563a9cd0bc2d7e04141ec72513b0baa8087484fd5f7bc`;
ELF SHA-256:
`1ede590a3e934ae44f88412b58d1082944087950eb8b5644f8eb1de88ce07f23`.
Daisy was not flashed; its running image identity was not established and
no audio result is claimed.

- **018a failed:** ESP32 build and USB-JTAG flash succeeded with all write
  hashes verified. Serial startup selected the 8-inch JD9365 driver but did
  not reach touch/UI initialization; the screenshot request timed out.
- User observed a blink after flashing followed by a black/blank screen.
  A clean single-reader capture reproduced the stop after the driver's
  `I2C Bus V2 uses the externally initialized bus handle` message.
- After reset and six seconds of execution, OpenOCD halted CPU0 at
  `0x480cc472`. The matching ELF resolves this to
  `mipi_dsi_hal_host_gen_read_short_packet`, waiting on the DSI read-busy bit.
  The panel initialization reads its ID before sending the vendor sequence.
  The debugger resumed the board afterward; its RTOS backtrace was incomplete,
  so this identifies the active wait rather than proving the underlying cause.
- Local evidence (gitignored): `logs/display-build-20260918.log`,
  `logs/display-flash-20260918.log`, `logs/display-boot-clean-20260918.log`,
  `logs/display-openocd-20260918.log`, `logs/display-backtrace-20260918.log`.
  **Follow-up:** Confirm separate panel power and ribbon seating/orientation,
  power-cycle, then repeat startup. Investigate reset/read behavior if the DSI
  wait persists. Touch, brightness, rendering and audio/load cases remain open.

**Same-day follow-up:** The user found the ribbon connected to the camera
connector. Moving it to the display connector restored startup without a
firmware change: JD9365 ID `93 65 04`, GT9271 identification and UI initialization
all succeeded. This resolves the original read-busy blocker. **018a passed**:
`logs/display-correct-port-20260918.png` is a clean 1280×800 Main Menu snapshot.
The user reports a stable scrambled physical image that changes slightly with
touch; **018b remains failed**. A blue screen during debugger/reset diagnostics
returned to scrambling after a normal restart.

Live driver inspection confirmed native 800×1280, 24-bit RGB888 input/output,
two framebuffers and 90-degree rotation. A full framebuffer debugger dump timed
out and is not usable pixel evidence; the board was reset to restore execution.
An isolated RGB565 diagnostic build retained the same geometry, rotation and
timing. Neither the clean RGB888 snapshot nor touch response validated physical
rendering or touch accuracy.

**RGB565 result, 2026-09-18:** The diagnostic build and USB-JTAG app flash
passed; the user confirmed the physical display now works. The image was built
from `a123f96` with an external RGB565 SDK configuration (16-bit LVGL and BSP
colour format), app SHA-256
`5f3ef236d41adfa9bee0db280865331215afe7ce9e6ba8fb3f2a570d1160d5a7`.
RGB565 is now selected in both tracked SDK configuration files. The comparison
isolates the failure to the RGB888 path but does not establish the specific
driver, PPA or scanout cause. **018b is partial:** readable physical output is
confirmed; corner accuracy, colours/edges and multi-contact release tests remain
open, as do brightness/wake and load/soak checks.

Local evidence: `logs/display-rgb565-build.log`,
`logs/display-rgb565-flash.log`, `logs/display-rgb565-boot-20260918.log`,
`logs/display-rgb565-working-20260918.png`. The original RGB888 image is retained
in `logs/display-rgb888-baseline/` for a controlled future comparison.

### UI framebuffer inventory, 2026-09-21

Captured and visually reviewed **59 native 1280 × 800 screenshots**, covering
all current UI pages/tabs plus mapping, persistence and confirmation views.
Gallery: `logs/ui-pages-20260921/index.html`; portable bundle:
`logs/ui-pages-20260921.zip`. Manifest records the UI state for each image;
`capture-info.json` records normal, profiling-disabled images matching HV-028.
Serial image transfers with missing data were retried; final PNG dimensions
and manifest references validate. Existing contrast, clipping, stale text and
Main Menu scrolling issues were subsequently fixed and recaptured below.
This confirms framebuffer contents only; physical panel/touch/encoder checks
and the phase gate remain open. No files were saved during capture.

**UI polish recapture:** reviewed eleven affected views in
`logs/ui-polish-20260921/index.html` (portable bundle:
`logs/ui-polish-20260921.zip`): Main Menu first/last items, MIDI Diagnostics,
Pad Map assignment/naming, Songs, all three new modulation destinations and
Step Notes live-record instructions. Header context now ends before the meters;
disabled Song controls and Pad Map dialogs retain the dark palette. MIDI cards
explicitly disclose unavailable diagnostic fields; counter wiring was still open
at this capture and is now implemented with acceptance tracked in HV-032.
Manifest/PNG validation passes at 1280 × 800. `capture-info.json` records
the per-image firmware identity: HV-029's HIL image, followed by its final
Songs-only style correction. Both are profiling-disabled. Missing serial image
data was retried. These captures establish framebuffer layout, not physical
contrast, touch feel or rendering/audio behavior under load.

## HV-019 — Note-group allocation policy

**Introduced:** Whole-note admission foundation, 2026-09-18.
**Design / gate:** [Phase 2.5 polyphony](roadmap.md#phase-25--sampler-instrument-layer),
[allocation model](features/project-menu-and-voice-model.md#instrument-and-kit-allocation-policy).
**Setup:** Matched persistent QSPI `-O2` profiling images, both boards, mono and
stereo resident WAVs, an Instrument with overlapping layers, audio capture and
the existing DWT logger. Preserve source fixtures and record exact image hashes.

- [ ] **019a — Before-integration baseline:** Use the replacement card with
  restored WAV fixtures. Run the existing full channel-budget stereo/Mono scenarios
  with both oscillators, modulation, locks, sequencing, streaming and file
  operations for at least ten minutes each. Separately exercise repeated-note
  and layered trigger bursts at capacity. Record complete profiling windows,
  event/render zones, UART pressure and stream underruns per scenario.
  **Pass:** Repeatable workload and image identity, zero underruns, callback
  maximum below the continuation threshold in [performance monitoring](performance_monitoring.md).
  An idle capture or host planner duration cannot pass this gate.
- [ ] **019b — Matched group integration:** After the callback owner and complete
  trigger handoff are implemented, repeat 019a unchanged. Add cap=1/4/Auto,
  Own only/Own first/Any, stereo and layered pressure, release tails, binding
  replacement and rejected triggers with choke groups.
  **Pass:** Whole notes admit or refuse; no partial layer steals/chokes, stale
  note-offs or ownership leaks. Capture audible steal/retrigger transitions,
  worst callback cost and stack/memory headroom; preserve the global budget.
- [ ] **019c — Saved policy and controls:** After persistence/Apply/Revert/UI
  integration, save/load/new/reboot Instrument and Project policies, including
  old files with explicit legacy values and Track inheritance. Change caps
  during held/releasing notes and test Mono fallback/repeated-key identity.
  **Pass:** Saved defaults and overrides follow the documented legacy policy,
  edits apply at a callback boundary, and lowered caps act at the next admission.
  Pad overrides are tested only if separately adopted.

**2026-09-20 saved policy / Mono integration:** Instrument/Kit defaults and
Track overrides now reach the immutable callback map and round-trip through
WXI/Bank/Project codecs. Host tests cover legacy defaults, invalid policy bytes,
retained completion/revisions, independent undo, ledger exhaustion, repeated
keys, global stealing, mode/binding retirement and one-shots. Static DTCM is
61,288 B, below the 64 KiB static cap that preserves the other 64 KiB for stack.

Two-board `tests/hil/test_polyphony.py` passes (17.14 s): Mono fallback,
FIFO repeated pitch, a Poly two-group Track cap, inheritance, Apply/Revert and
unique Project save/new/load restoring both sound defaults and Track override.
Evidence: `logs/polyphony-hil-final.log/.xml`; normal Daisy SHA256
`95dd34cf715e35625409cf9273f06dc9503fe1d76f838ac5659639eebfaa88bd`,
ESP32 `09c527f75ba21eef9ce24b03a5372e47418724cba99bccbd636e50c76577c936`.
Final normal-image regression: **10/10 passed in 69.59 s** across
`test_polyphony`, `test_live_note_identity`, `test_sequencer_tracks` and
`test_note_groups` (`logs/polyphony-final-image-hil.log/.xml`). Images: Daisy
`0982375b0b21044b3511db483094ffc42c6bb08ecf0dca336f08a2325d917fb2`, ESP32
`3d17b704ca294b0e9f6db15d4bf17bb5a3aad6d852a513e7486967b259754e91`.

Earlier fixture/setup and save-wait failures are preserved in
`logs/polyphony-hil*.log`; the final test waits for backend-confirmed save identity
and starts with a reset bench Pool. This checks injected touch events/readback,
not physical MIDI, reboot recovery or audible transitions. The actual LVGL
Track Polyphony snapshot (`logs/polyphony-track.png`, 1280×800) was inspected:
labels/values, focused field, guidance and softkeys fit without clipping. This
is framebuffer-content evidence, not a physical panel/touch or frame-time gate.
Those portions of 019c remain open. The short Mono fallback pressure screen
passes with 36.5081% average / 69.5417% maximum callback load and zero queue
refusals or stream underruns (131 injected note-ons, 81.14 s, one Pattern cycle).
See [the exact images and capture](callback-performance-log.md#saved-policy-and-mono-held-key-fallback--2026-09-20);
the same-image Poly screen is 36.1145% average / 66.3496% peak, zero refusals
(68 note-ons, 80.09 s, one Pattern cycle). Extended timing acceptance is still open.

**2026-09-20 admission-batch result:** Same-frame events now plan from compact
prepared zone metadata and materialize only surviving layers. The former
16-Track/four-layer overrun passes 605.86 seconds / ten Pattern cycles at
35.35% average and 66.0025% maximum; boot/setup maximum is 66.8390%, with zero
stream underruns and sampled console RX drops. Candidate Daisy SHA256
`ac4f254f5d2b1b3570ab37da041701a36cac47a9d79b11fde7ece401002cb736`,
ESP32 `82fbc61818891bfaf1885f52213e6a67d09c277f3c7a8a21761a0bcb16b38a1a`.
779 host tests and 22 targeted sanitizer cases pass, including sequential/batch
voice and rendered-audio equivalence, channel/choke metadata and sample offsets.
See [the measured workload and captures](callback-performance-log.md#same-frame-admission-batching--2026-09-20).
This resolves the reproduced layered deadline failure only. Preserve the earlier
mixed-channel transition evidence below; final-image one-hour soak, physical
MIDI/listening and saved-policy acceptance remain open. The user permits
continuation with these residual checks recorded in the roadmap.

**2026-09-20 compact live-input result:** The matched 80.024-second screen
with 68 MIDI fan-out bursts and one Pattern cycle has zero queue refusals,
zero stream underruns, 34.8683% average and 62.1127% peak. The preceding version
refused 540 notes in this same scenario. See
[the image identities and capture](callback-performance-log.md#compact-live-note-admission--2026-09-20).
Eight new host cases exercise FIFO repeats, source isolation, old/stolen/refused
presses, routing changes, overflow offs and pending-event invalidation on Track
replacement. The focused 35-case host selection and normal firmware build pass;
DTCM statics use 59,712 bytes, below the 64-KiB stack-preserving link cap.
Long-duration capacity, physical MIDI, Mono fallback and saved-policy gates
remain open.
Normal image SHA256 `14f03ea100319f9cf065258366ee63f6bcf412014b686938b1ea086da2266552`
passes the three new two-board key-identity checks (repeated/source-separated
keys, MIDI routing change, and old release after binding replacement), both
layer-group checks and the selected sequencer retirement checks. The initial
ready-board selection had eight passes and one old test assumption: a live
off used to release a sequencer voice. The corrected sample-edit test preserves
existing scheduler-owned snapshots and verifies subsequent hits finish at the
edited bounds; it passes separately in 4.01 s. Evidence:
`logs/live-notes-hil-ready.xml` / `.log` and
`logs/live-notes-edit-retest.xml` / `.log`. An immediate post-flash attempt
skipped all nine tests before USB re-enumeration and supplies no validation.

The same candidate also passed a 605.2-second rapid rotation through all five
channel mixes, switching every ten seconds with ten Pattern cycles. Average
30.3113%, maximum 59.9650%, zero stream underruns and sampled console RX drops;
`logs/stereo-0-ladder-20260920-150155.log` / `.json`. This did not reproduce the
earlier transition peak and does not close the final-image one-hour soak.

**2026-09-18 result:** Ten host planner tests pass, including exhaustive
four-slot feasibility/resource checks and the 64-slot mask boundary. The
default eight-slot planner compiles for Cortex-M7 with C++17, `-O2`, exceptions
and RTTI disabled. Daisy build/test hooks pass. It is not linked into the live
allocator. The user replaced the empty formatted card with a sample-filled card;
`/03 Lips of Ashes.wav` is readable and the current audio path was flashed as a
profiling image for 019a. Group lifetime/held-note integration, saved policy and
controls are not implemented. Physical acceptance remains open pending the
recorded complete workloads below.

**2026-09-18 baseline — 019a partial / REVIEW:** The replacement card ran
`/03 Lips of Ashes.wav` with eight Mono render voices at the complete eight-channel
budget, two oscillators, ladder/drive, modulation, four locks per hit, sequencing,
stereo streaming and ten unique Pattern save/load cycles. The workload completed
in 608.77 s; 122 captured DWT windows span 610.2 s. Zero stream underruns and zero
sampled console dropped bytes. Callback average 175,783 cycles (36.6%); maximum
336,019 / 480,000 cycles (**70.0040%**), so the automatic gate is **REVIEW**, not
STAY. The same window reports event-zone maximum 164,825 cycles and render-zone
maximum 164,303 cycles; these window maxima do not establish coincident costs.
Review/profile event handling and repeat the matched workload before adding
allocator work. Stereo/mixed scenarios and layered/repeated-note bursts remain
unrun in this session.

Image: Daisy persistent QSPI `-O2`, profiling On, SHA256
`778b0c77d07ae328733568199b41b1e69ab9960c49b76723fecacaf32b41242f`;
ESP32 running ELF prefix `1ede590a3` (2026-09-18 17:50:39 build).
Evidence: `logs/stereo-0-ladder-20260918-233158.log` and `.json`,
`logs/polyphony-baseline-run2.log`, and the recorded
[performance gate row](callback-performance-log.md). The earlier 60-second
attempt ended on stale harness navigation and is excluded from acceptance.

**2026-09-18 group integration — functional checks, capacity pending:** The
callback now admits complete layer groups and owns their stable IDs and Track
binding generations. Foreground note queues publish all layers atomically;
sequencer hits use the same group admission. Host coverage includes partial
layer completion, whole-group stealing, rejected-trigger choke isolation,
stale releases, one-shot layers and local cap accounting. The group integration source passed 771 Daisy tests and 37 selected
ASan/UBSan cases; both output configurations compile.

`tests/hil/test_note_groups.py` passes both live-key and sequenced cases on the
two boards (45.20 s): construct four overlapping keyboard zones, fire the
four-layer note, fill the remaining channels with four independent notes,
then trigger one more note. All four old layers retire together: the active
count falls from eight to five. Scoped note-offs leave unrelated notes intact.
Daisy profiling image SHA256
`d86a89be2a289118e72561879b9753c9bd64575ae12329909726128e04b97f27`;
frontend SHA256
`be9965b1aa45707366d352c21ef4f4386b51dc01a1a080df2b08900f8cfdfb4e`.
Evidence: `logs/note-groups-first.xml` and `logs/note-groups-first-run.log`.
This image's short capacity screening reached 73.90% and was interrupted for
planner placement work; functional passes do not clear that performance gate.
Saved policies, held-key fallback, listening and the full 019b/019c checks remain
open. Repeat the four-layer procedure with stereo layers and under burst/load
pressure before closing 019b.

**2026-09-18 final capacity repeat (UTC Sep 19):** The eight-Mono workload
completed 610.5 seconds / 122 DWT windows and ten Pattern file cycles, with zero
stream underruns and console RX drops. Weighted mean 174,512 cycles (36.4%),
maximum 338,778 (70.5787%, **REVIEW**); setup maximum 348,202 (72.5421%). This
covers only the eight-Mono subset of 019a; mixed/stereo and burst workloads,
listening, interrupt-entry latency and the one-hour phase soak remain open.
The candidate includes whole-note admission, prospective choke priority,
startup pitch lookup, LFO configuration reuse and planner/filter ITCM placement.
Daisy SHA256 `386259b62c24a2dd338e441026be99b939b739fd3729fe1655fb830a29eb0e5c`;
ESP32 unchanged as above. Evidence:
`logs/stereo-0-ladder-20260919-033711.log` / `.json` and
`logs/callback-final2-run.log`. See [timing evidence](callback-performance-log.md)
for the earlier candidates; no net peak improvement or complete gate is claimed.

**2026-09-18 correlated profiling and final regression (UTC Sep 19):** Build
with both `WAVEX_PROFILING_ENABLED=ON` and `WAVEX_PROFILE_CALLBACK_DETAIL=ON`,
then run the eight-Mono benchmark for 185 seconds including three Pattern
save/load/restart cycles. Inspect `callback_peak` and all `peak_stage` records
with the same window ID, including setup; repeat acceptance with detail off.
Diagnostic pass criteria are intact correlated records, eight matching trigger
calls in the peak, zero reported stream underruns, and no use of this overhead-
bearing image as normal timing-gate evidence. These diagnostic criteria passed;
[the stage table](callback-performance-log.md#correlated-peak-attribution-diagnostic-image)
records the setup and workload peaks. No capacity or phase gate closes here.
Image SHA256 `c65fe60a19cbb3d36d8a88b6d46bfe22d47502ddbdc01504fe0187d0a454a716`;
`logs/callback-detail-boot.log`, `logs/callback-detail-peaks.json`, and
`logs/stereo-0-ladder-20260919-035249.log` / `.json` retain the evidence.

Normal profiling-off Daisy image SHA256
`afa12b533956996e4ccb1a0af720f1b195c62f4aa4118b37066608b5ded959ec`
was restored afterward. The final source passes **773 Daisy host tests**, **40
selected ASan/UBSan cases**, and both report-helper tests; Stage A, Stage B and
the optional detailed profiling image compile. The combined two-board HIL
rerun is **incomplete**: three Daisy sequencer cases passed, twelve UI-dependent
cases skipped because a concurrent frontend session changed its console to
USB; the retry reached the first group test but lost its console acknowledgement
during a further ESP32 reflash. This is not a new group-allocation failure or a
passing final regression. Preserve `logs/callback-final-hil.xml`,
`logs/callback-final-hil-retry.xml` and their `*-run.log` files. Repeat the
15-case group/stereo/sequencer selection with stable exclusive bench access.
The earlier two group passes above remain valid for their recorded image.

**2026-09-19 sequencer ITCM iteration:** Retain the measured placement of
scheduler start/step and transport hot paths in ITCM (35,680 / 65,536 bytes).
The full control and candidate each captured 605.2 seconds, and a subsequent
185.1-second unchanged-control return check reproduced the higher cost.
Candidate average/peak were 35.1% / 64.76%, versus 36.7% / 71.15% for the full
control; setup improved from 72.98% to 65.99%. No stream underruns were observed.
See [the exact images, counters and comparison](callback-performance-log.md#sequencer-placement-trial--2026-09-19).

The candidate harness failed in its tenth Pattern cycle: the UI performed Back
navigation, but native-USB console ACK 940623 was absent. Nine cycles were
confirmed and the full timing capture is retained, but this is **not** a complete
workflow/phase pass. The short control repeat completed all three file cycles.
The native-USB console issue remains open in the roadmap; do not silently replay
mutating commands when their acknowledgement is missing. Repeat the full
workload on the exact clean commit before using it for a phase/release decision.

Normal profiling-off Daisy image restored:
`155a07258a991511524c92eab1a93f6310661e5dd30f23919d9752147a2c8982`.
Frontend image:
`2c1f693d3a0dfdbd369b36e74cedc06ee22c4b55c0d1e9d3e574a742ba5a5403`.
All 773 Daisy host tests and the repository build/test hooks pass; Stage B also
compiles. Normal-image HIL initially stopped on a missing `STATE` ACK before
its first audio assertion (`logs/seq-final-hil.xml`). A read-only probe recovered;
the retry passed nine group/stereo cases, then failed when the frontend rejected
`PAGE MUTE 0` as unknown (`logs/seq-final-hil-retry.xml`). The separate sequencer
selection passed all four cases in 22.37 seconds (`logs/seq-final-tracks.xml`).
These partial results do not complete the combined regression or capacity gate.
Resolve the console failures and finish the remaining stereo workflow checks;
mixed/stereo capacity, repeated-note bursts, interrupt-entry latency, listening
and the one-hour soak stay open.

**2026-09-20 follow-up:** The previously interrupted normal-image regression
now passes in the 26-case selection recorded at HV-020/HV-021 below. The unchanged
profiling image `f6c8a1957f89e08a590d989462aac553dd8cfd3a559914ba532068b8bb3c04fa`
also passes the full two-stereo/four-Mono workload: 605.3 s of DWT windows,
ten Pattern save/load cycles, zero stream underruns, average 30.2%, workload
peak 56.0135% and boot/setup-inclusive peak 56.85%. See
[the capture and image identities](callback-performance-log.md#mixed-channel-sequencersampler-validation--2026-09-20).
This closes that functional regression and mixed-channel capture, not HV-019:
other mixes, burst/latency checks, listening, the one-hour soak and clean-commit
repeat remain open. A Daisy-only reflash also exposed a peer-restart/browse
failure; restarting the frontend recovered it, with later fixes/results at HV-022.
After restoring normal profiling-off Daisy firmware and restarting the frontend,
the final smoke check found ten root browse entries, a ready stopped Sequencer,
zero voices/streams/underruns and zero dropped replies; the UI was left at the
main menu (`logs/seq-sampler-final-smoke.log`).

**2026-09-20 full-mix follow-up — 019a capacity failure:** All five mixes and
a return-to-Mono segment now have more than ten minutes of DWT windows in the
61-minute zero-stream-underrun run linked at HV-005. Its final transition/file
cycle raised the overall peak to 74.5896%; this supersedes the earlier lower
steady-state peaks for capacity acceptance. Detailed transition attribution
did not reproduce that high window. The two four-layer repeated-note runs
subsequently pass ten minutes each (Mono peak 57.1544%, stereo peak 43.4917%;
20 combined Pattern cycles and 1,009 foreground-injected MIDI notes).
A separate 16-Track/four-layer pressure screen fails the deadline: 114.5108%
workload peak, 118.0392% including setup, with only eight render channels.
Zero stream underruns do not clear that failure. Saved policy, held-key
ownership, listening, interrupt-entry latency and clean-commit repeat remain
open; the full 019b/019c gates are not satisfied by the soak.

## HV-020 — Complete kit and pad-pattern workflow

**Introduced:** Full-kit HIL, 2026-09-18.
**Design / gate:** [Phase 2.5 kit workflow](roadmap.md#phase-25--sampler-instrument-layer),
[Track/Instrument model](features/track-and-patch-model.md),
[sequencer](features/sequencer.md).
**Setup:** Both boards, two short distinguishable WAVs on a backed-up card,
unique Instrument/Pattern names, serial and audio capture. The automated case
replaces the live session and preserves saved source files.

**2026-09-20 regression:** The complete 16-pad WXI/Pattern round trip passes
in the combined 26-case console/group/stereo/sequencer selection
(`logs/seq-sampler-final.xml`, 192.37 s). This includes all four routing/retirement
cases, live and sequenced four-layer stealing, all five channel mixes, Mono
changes while held, WXI Mono recall, Project gain/pan/mute and Solo preserving
manual mutes. These are digital meters and state assertions, not listening or
reboot/failure-recovery acceptance. Daisy normal image SHA256
`155a07258a991511524c92eab1a93f6310661e5dd30f23919d9752147a2c8982`;
ESP32 SHA256
`78c63611b1f5425d41639b0211d5969b83eb25925655e1fc54ffe4ebd6dfbffe`.
The kit case took 60.22 s and recalled `HIL16 74033046462253` at directory
index 90, providing another populated-directory result for 020d.

**2026-09-20 final normal-image regression:** Daisy profiling/detail/RTT Off
image `0e5ed82752a443dbc1cfae35f3d175045c06ab28334d53427d0a5737600e87aa`
and frontend `82fbc61818891bfaf1885f52213e6a67d09c277f3c7a8a21761a0bcb16b38a1a`
ran the final console, group, stereo, sequencer, kit and pagination selection.
The initial run had 26 passes and one kit-test failure in 176.25 seconds:
the test redundantly touched Saved while already in that directory, then
selected before the asynchronous touch refreshed the list. The test now skips
that redundant navigation and requires the selected filename alongside Load
readiness. The focused full-kit repeat passed in 60.70 seconds.
A second full-kit repeat with Saved already restored passed in 60.39 seconds,
exercising the previously failing branch without a second Saved touch
(`logs/batch-kit-restored-retest.xml` / `.log`).
Evidence: `logs/batch-final.xml`, `logs/batch-final-hil.log`,
`logs/batch-kit-retest.xml`, and `logs/batch-kit-retest.log`. Preserve the failed
attempt; these functional checks do not close listening, reboot or failure
recovery. The same source passes 779 Daisy and 450 shared host cases, four Python
checks with nine subtests, and both output configurations compile.
Final cleanup stopped sequencing/streaming, cleared the bench's RAM Tracks and
samples, and returned the frontend to Main Menu: zero active voices, stream
underruns, console drops and reply drops; root listing has ten entries and the
Sequencer reports ready/stopped (`logs/batch-final-smoke.log` / `-result.json`).

- [ ] **020a — Complete kit:** Run `tests/hil/test_kit_sequence.py`. Assign all
  16 pads, alternating two resident samples and setting choke on even pads.
  Save a WXI copy, load it onto another Track and inspect every pad assignment
  and choke value while a separate Track retains its held note.
  **Pass:** Every mapping survives the copy; the other Track remains bound
  and sounding. Duplicate-save and cancel behavior is additionally covered by
  `tests/hil/test_kit_editor.py`.
- [ ] **020b — Pad-addressed pattern:** Program 16 steps on the recalled Kit's
  Track, addressing pads 1 through 16. Save/New/Load the Pattern and inspect
  all enabled bits and note values. Start/Stop and listen through a full loop.
  **Pass:** The expected alternating sounds play in order, choke affects only
  the intended Kit, Pattern recall preserves Track bindings, and no underruns
  or stuck notes occur. Automated voice-count readback is not a listening test.
- [ ] **020d — Populated saved directory:** With more than 50 WXI entries,
  save a uniquely named Kit after the first 50 directory positions, browse to
  it, select it and recall all pad assignments. Re-enter the browser and repeat.
  **Pass:** The later Kit remains reachable; loading preserves other Tracks.
  Host pagination also covers index 255 and the final partial page. The
  500-entry target remains outside this protocol limit.
- [ ] **020c — Recovery:** Reboot both boards, reload the Kit and Pattern and
  repeat 020b. Exercise missing dependencies, card removal during Save and
  insufficient staging memory, preserving the earlier saved copies.
  **Pass:** Successful copies reload; failure never installs a partial Kit or
  overwrites source files. Record image identities and per-case evidence.

**2026-09-18 browser and supporting regressions:** The replacement card exposed
a real 50-entry frontend cutoff: 86 saved Instrument entries existed, but only
50 were reachable. Both browsers and the Daisy index cache now use the existing
256-entry listing limit. A host regression pages through all 256 entries and
selects index 255 without wrapping. Both firmware builds and 1,570 host tests
(450 shared, 362 frontend, 758 backend) pass. No 500-entry support is implied.

On normal QSPI `-O2` Daisy firmware with profiling Off, eight supporting HIL
cases pass: kit save/recall/overrides, parameter locks, Track routing/retirement
and grid editing. `logs/kit-sequencer-final.xml` also retains the full-kit test's
initial failure: it pressed Load during reference inspection. That harness
now waits for Load to become enabled, in addition to waiting for paginated
entries and save completion; no load command is retried blindly.

The corrected full-kit case passes in **117.95 s** (`logs/full-kit-final.xml`):
`HIL16 75321629692998` was selected at directory index **88**. All 16 alternating
sample assignments and choke values survive WXI recall onto Track 3; another
Track retains its binding, with its held voice preserved during kit editing.
All 16 enabled steps and pad notes 60–75 survive Pattern Save/New/Load, followed
by two loops at 120 BPM, Start/Stop and zero reported underruns/dropped bytes.
Together with the eight supporting cases, all nine selected HIL cases pass.
These are automated readback/playback checks; 020a/b/d have supporting bench
evidence, while listening and 020c recovery remain unverified.

Tested images: Daisy SHA256
`69228096000d06074a22a5561173e93662816e1025e1b2a27a47677fa1ff729c`;
ESP32 app SHA256
`be9965b1aa45707366d352c21ef4f4386b51dc01a1a080df2b08900f8cfdfb4e`.
Build/flash and checks: `logs/kit-browser-precommit.log`,
`logs/kit-browser-precommit-final.log`, `logs/kit-hil-python-checks-final.log`,
`logs/kit-final-text-checks.log`, `logs/kit-browser-daisy-flash.log`, and
`logs/kit-browser-esp32-flash.log`. Formatting findings from intermediate runs
were corrected; all affected checks subsequently pass. Physical audio capture,
reboot/failure recovery and the complete Phase 2 gate remain open.

## HV-021 — USB console and Daisy RTT

**Introduced:** Debug transport experiment, 2026-09-19.
**Design / gate:** [Logging](logging.md), [Phase 2 timing gate](roadmap.md),
[debug console architecture](architecture.md#debug-console-transport-as-built-2026-09-19).
**Setup:** ESP32-P4 native USB Serial/JTAG, Daisy USB CDC, ST-Link V2
(firmware V2J29S7), devcontainer, both serial loggers. Save normal images;
build the optional RTT QSPI image using the pinned source in `logging.md`.

- [x] **021a — ESP32 console:** Resolve the native USB port, flash with a
  managed logger running, then run `tests/hil/test_console.py`. Capture and
  decode the same main-menu screenshot over bridge UART and native USB.
  **Pass:** Commands, logs and CRC-checked screenshots work; logger resumes
  after flashing. Pause its reader, request a screenshot, resume and verify
  fresh PING/STATE replies without resetting the board.
- [ ] **021b — RTT integrity and delivery:** Run `scripts/bench_daisy_rtt.py`
  for 100 bursts with the documented OpenOCD server; repeat after reboot.
  **Pass:** Every USB/RTT payload matches, loss counters remain unchanged,
  and complete-burst latency is recorded. **Failed** on the current probe.
- [x] **021c — Absent RTT reader:** Stop RTT polling, issue 80 `LOG ?`
  commands via USB, and compare LOGSTATS/STATE before and after.
  **Pass:** RTT loss increases without USB loss or stalled callbacks.
- [ ] **021d — Active audio:** Repeat 021b/c during resident and streaming
  playback, capturing callback timing and listening for faults.
  **Pass:** No new underruns, command stalls or audio faults; callback timing
  remains within the phase gate. **Deferred:** resolve 021b before promotion.

**2026-09-19 bench:** Native ESP32 JTAG detected both HP cores without halting
or resetting. Native USB console firmware flashed successfully, including an
app reflash while the managed logger was active. Seven console HIL cases
passed. A 1280x800 main-menu screenshot took **16.071 s via UART** and
**1.568 s via native USB** (repeat **1.564 s**), approximately 10.3 times faster
end to end. These are capture timings, not CPU or raw transport benchmarks.
Reader-pause/reconnect PING and STATE checks passed.

The SRAM RTT build exceeded D2 by **22,888 bytes** (285,032 / 262,144) and
DTCM by **528 bytes** (98,832 / 98,304, with stack space reserved). The separate
QSPI RTT build fits without changing linker limits. It used 474,960 bytes AXI
SRAM, 28,808 bytes D2 DMA, 69,568 bytes general D2, 9,376 bytes DTCM and
28,792 bytes ITCM in this dirty-tree test image.

RTT readout failed with OpenOCD 0.12.0 HLA and newer native ST-Link DAP, also
after explicitly enforcing the M7 publication barrier. At requested 400 kHz
(actual **240 kHz**) SWD and 10 ms polling, **32 identical 331-byte bursts**
completed before corruption. Their median complete-burst delivery was
**USB 2.499 ms / RTT 49.951 ms**. RTT loss stayed at 5,562 and USB loss at zero
during this trial. A direct RTT RAM dump contained correct text missing from
the received stream, suggesting a probe/readout-path fault; the cause is not
proven. RTT therefore remains opt-in; the USB ring is retained.

With polling stopped, RTT loss rose **5,562 → 31,945**, USB loss and ISR-log
attempts stayed zero, callback blocks advanced **248,624 → 250,380**, and
underruns stayed zero. This was **idle audio (zero voices/streams)** and does
not satisfy 021d. The saved normal Daisy image was restored after testing.

Tested image SHA256 identities (working tree; includes concurrent audio work):

- ESP32 USB app: `0dfd8024303395c249d7e94f929e886e4b830c06cdeb76a71c340bad312c0317`.
- ESP32 UART baseline: `be9965b1aa45707366d352c21ef4f4386b51dc01a1a080df2b08900f8cfdfb4e`.
- Daisy QSPI RTT: `0643bdeca640b54f56e8412f46d84f506eef0615c0ea89aafdb2440adc4e723e`.
- Daisy normal restored: `afa12b533956996e4ccb1a0af720f1b195c62f4aa4118b37066608b5ded959ec`.

Evidence is retained locally under `logs/usb-rtt-20260918/`: build/flash and
restore logs, `esp32-jtag.log`, `uart.png`, `usb.png`, `usb-final.png`,
`rtt-400khz.json`, `rtt-400khz-error.log`, `rtt-buffer.bin`, and
`rtt-no-reader.json`. These generated artifacts are gitignored. Repeat RTT
integrity with a different probe/host stack before removing any USB buffering.

**2026-09-20 sustained reply regression:** The prior frontend image
`2c1f693d3a0dfdbd369b36e74cedc06ee22c4b55c0d1e9d3e574a742ba5a5403`
lost one of 400 consecutive Key Map STATE acknowledgements
(`logs/console-state-baseline.log`), reproducing the workload blocker without
replaying any mutation. The replacement image
`78c63611b1f5425d41639b0211d5969b83eb25925655e1fc54ffe4ebd6dfbffe`
enqueues each reply as one driver write and reports rejected enqueues through
`reply_dropped`. Both successive HIL attempts passed 400 Instrument STATE replies
interleaved with 400 PING replies, with no rejected enqueue. The complete final
selection passes 26 cases; ESP32 build and all 362 host tests pass.

The earlier `PAGE MUTE` rejection was a test readiness race, not a missing
command: the test sent its next edit before confirmed mixer readback. A second
race issued cutoff immediately after entering Filter. Both tests now wait for
readiness and the confirmed value. Preserve failed attempts in
`logs/seq-sampler-pass.xml` and `logs/seq-sampler-usb.xml`, and the successful
`logs/seq-sampler-final.xml`. This evidence does not establish arbitrary USB
disconnect recovery or close the Daisy RTT gates.

## HV-022 — Peer restart and browse delivery

**Introduced:** Restart/backpressure recovery, 2026-09-20.
**Design / gate:** [UART transport](features/inter-mcu-protocol.md),
[Phase 2](roadmap.md#phase-2--groovebox-core-sequencer-and-pads).
**Setup:** Both boards, managed USB loggers, a persistent Daisy image and the
replacement card fixtures: `/` (10 entries), `/Drums/Kicks` (51) and
`/Drums/Loops` (28). The bench script replaces the current unsaved session.

- [x] **022a — Isolated restarts:** Run `scripts/bench_peer_restart.py --image
  firmware/daisy/build-recovery/wavex-daisy.bin --cycles 3` inside the hardware
  devcontainer. It leaves the other MCU running, completes each paginated
  listing, loads the kick fixture, checks note admission/release and waits for
  Sequencer readback after each reset.
  **Pass:** All six restarts recover without restarting the other board; exact
  fixture counts, sample load, note release and Sequencer readiness succeed.
- [x] **022b — Browse pressure and selection:** Repeat the capacity harness
  setup, which generates status traffic, and select an early-page sample while
  later pages arrive. **Pass:** The listing arrives after TX backpressure;
  pagination preserves the selection and Load addresses that file.

**Initial failures:** The earlier empty-root capture explicitly reports
`Failed to send browse response (queue full?)`; its combined overflow count
was not evidence of Daisy RX loss. A retained main-loop response now retries
that enqueue. Subsequent reset testing exposed selection reset on final-page
arrival and an ESP32 hardware FIFO overrun (`UART overflow (3)`) that lost a
root listing. Preserve `logs/peer-restart-20260920-041252.*` (selection) and
`logs/peer-restart-20260920-041751.*` (FIFO loss). The logger-restart test also
exposed a host tailer stuck past EOF after in-place truncation; that is a
harness failure, with board replies present in the raw log.

**2026-09-20 restart result:** All six isolated resets passed after the transport
and selection fixes. Capture `logs/peer-restart-20260920-042644.log` / `.json`
records exact 51/10/28/10 entry counts on every cycle, successful sample load,
note admission/release and Sequencer readiness. Daisy profiling image:
`bbb29c9e2ef70a4708829871344ce5076c777f6c7a568b0670cd0d58146495ac`;
ESP32 image: `82fbc61818891bfaf1885f52213e6a67d09c277f3c7a8a21761a0bcb16b38a1a`.
No ESP32 FIFO-overflow log occurs in that final capture. Daisy host tests:
776 pass; ESP32 host tests: 362 pass. Both Stage A and Stage B compile.
The two log-tail restart/rotation tests pass. Earlier failed attempts remain
preserved rather than counted as successful resets.

**Normal-image regression:** Daisy profiling-Off/detail-Off/RTT-Off SHA256
`45fed7701b849639d4d911a5f13fef7ef997b2d8b6e83b3fe600087ae39d0660`,
with the same ESP32 image, passes all 26 existing console, note-group,
stereo/Mono, sequencer, Solo and complete-kit cases in 192.48 seconds
(`logs/recovery-capacity-final.xml`). The new pagination case initially skipped
because STATE polling missed the early selection. Its ordered UI event log
showed selection before the final page; the focused test now requires that
ordering and verifies the selected file's successful load and Track binding.
It passes in 2.82 seconds (`logs/recovery-browse-selection.xml`). Thus all 27
selected cases have passed across the combined run and focused rerun.
The repeated capacity setups also completed their full listings under normal
status traffic. Host coverage separately forces outbox enqueue failures and
checks that later successful enqueue preserves the complete response.

**Final state:** Normal firmware remains installed. The bench session was
cleared, root browsing returned ten entries, and the Sequencer reported ready
and stopped. Daisy reported zero voices, resident samples, streams, underruns
and console RX drops; the frontend reported zero rejected replies. The UI was
left at Main Menu. Evidence: `logs/recovery-capacity-final-smoke.log` and
`logs/recovery-capacity-final-smoke-result.json`. No source WAV was modified.

The browse wire format still lacks request/path correlation. These checks
finish each paginated request before navigating again; they do not establish
correct cancellation under overlapping directory changes or recovery from
arbitrary dropped/late packets.

## HV-023 — Stream read recovery cursor

**Introduced:** 2026-09-20. **Gate:** [streaming CRC recovery](roadmap.md#streaming-crc-recovery).
**Setup:** disposable/read-only WAV fixture, trimmed region with an internal loop,
Daisy logs and a foreground FatFs read-failure injection build. Preserve the
source file and record the tested binary hashes.

- [ ] Start streaming a region whose end precedes the end of the WAV data chunk.
  Record the absolute offset before a regular slot read. Inject enough failed
  reads to enter the existing reopen/retry path, without consuming their bytes.
- [ ] Verify the recovery log seeks to that failed read's exact offset, retries
  the same frames, and retains channel order. Repeat after a loop rewind, with
  stereo PCM16 and PCM24 fixtures and a loop start inside the selected region.
- [ ] Force reopen/seek failure and confirm bounded abort and responsive controls.
  Record stream underruns, ring low-water and foreground service latency.

**Pass:** no skip to the full-file tail, no stale or duplicated slot publication,
correct region/loop continuation, and no unbounded retry. A compile or an ordinary
error-free playback run does not satisfy fault-injection or latency acceptance.
**Current result:** code retries the captured `read_at` position instead of
subtracting region-relative bytes remaining from full-file data size. The
injected-failure bench procedure is unrun; a deterministic injection build is
still required. The broader CRC-recovery deadline decision remains open.

## HV-024 — Melodic sequencing

**Introduced:** 2026-09-20. **Design / gate:**
[melodic sequencing](features/melodic-sequencing.md),
[Phase 2.5](roadmap.md#phase-25--sampler-instrument-layer).
**Setup:** Both matching images, the current SD fixtures, one looped keyboard
Instrument and a short drum sample, MIDI keyboard, audio capture and DWT logger.
Use unique Pattern/Project save-copy names; preserve source assets.

- [ ] **024a — Lane/editor/persistence:** Open Sequencer → Notes; edit all four
  lanes including MIDI note zero, velocity-zero clearing, finite gates and Hold.
  Switch drum/melodic, edit hidden steps, save/new/load Pattern and Project.
  Verify old files default to drum/empty lanes. Exercise reconnect and Pattern
  replacement while reads/edits are pending. **Pass:** confirmed values and
  ownership remain correct; no stale editor changes a replacement Pattern.
- [ ] **024b — Gate/ownership:** Sound a four-note chord with positive gates,
  then holds. Change tempo, overlap positive gates, mute/unmute, Stop/restart,
  locate, launch Patterns and advance Song sections. Add an independent held
  live key and force stealing/rebinding. **Pass:** exact gate timing in capture,
  bounded release tails, no stale releases and no stuck melodic notes; the
  independent live key survives sequence Stop.
  Include a Song whose melodic/enabled rows differ from the previously edited
  Pattern and change between sections: held notes must survive beyond one audio
  block and release only at their gate or section boundary. Capture DWT peaks
  for the same Song before/after the 2026-09-22 cleanup fix (not yet measured).
- [ ] **024c — Recording:** Record a two-bar four-note progression over drums
  from physical MIDI. Check quantize on/off, first-note shared microtiming,
  repeated pitches, a fifth-note replacement, step-record cursor advancement,
  held-pitch erase and mode/target changes. **Pass:** correct saved pitches,
  velocities/durations, deterministic replacement and matching source releases.
- [ ] **024d — Soak and UI:** Loop the recorded progression for ten minutes,
  inspect/listen to the physical panel/audio and capture DWT maxima. Run the
  full-load `--melodic` callback bench separately with streaming, edits and file
  cycles. **Pass:** no underruns/stuck notes, readable responsive panel, and
  callback acceptance under the documented capacity workload. Short screens
  and preprogrammed progressions do not pass the physical recording gate.

Automated coverage is `tests/hil/test_melodic.py`; its slow test runs a
preprogrammed four-note progression plus drums for ten minutes. Physical MIDI,
listening and the full Phase 2.5 gate remain separate from these checks.

**2026-09-20 initial functional result:** `test_melodic.py -m "not slow"`
passed in 20.14 s on Daisy
`cd988297778beb0e411dc8e3af14b6e6e214a1a1c2c5f18f2e798a6227286fdf`
and ESP32
`8f76bc763cc9ef2ce3992262ea92e99f519b207c8e7b414594364f53af5468d8`.
Evidence: `logs/melodic-hil3.log/.xml`. This covers four-note gates/holds,
sequence Stop preserving a live key, step/live capture, erase and Pattern
recall via the real UI/UART/SD. The first attempt found an ambiguous Play/mode
button label (fixed). Its unbalanced test key caused the next attempt to
consume that older press's FIFO release; the harness now balances owned keys
even after assertion failures. Neither failed attempt is counted as a pass.

The initial framebuffer inspection caught a clipped gate-unit label; the label
was shortened before final visual verification. Physical MIDI, listening,
Project reboot recovery and the full acceptance checkboxes remain open.

**2026-09-20 first normal-image regression:** **11/11 passed in 91.25 s** across
melodic, saved polyphony, live-note identity, note groups and sequencer Tracks.
Evidence: `logs/melodic-final-hil.log/.xml`. Normal firmware SHA256 identities:
Daisy `2ec52b185dfa4bd956ec49212b8e6cfaa2ccf3d40f56236111ff7dff767194b6`,
ESP32 `7fe03c7e5758375427784250a5dc00353ebe3f4486958e79cb110f80688c6d12`.
This source also rejects pre-rebind recording events and prevents old
held-capture releases from overwriting manual lane replacements. Shared 457,
Daisy 807 and ESP32 367 host tests pass; both Stage A and B firmware build.

The corrected Notes and parent Sequencer framebuffer captures are
`logs/melodic-notes-final.png` and `logs/melodic-sequencer-final.png`.
[UI profiling](ui-latency-notes.md#step-notes-follow-up--2026-09-20) measured
zero idle redraws and no full-screen refreshes for lane edits/selections;
local edits average 53.40 ms per submitted refresh. Profiling is disabled in
the final normal firmware. This is not physical touch or audible timing proof.

**2026-09-20 musical soak:** the preprogrammed four-note progression plus drum
test passed in **606.08 s** (600 s playback), using the same normal images as
the 91.25 s regression above, before the later half-step/feedback additions.
There were 304 state replies, a maximum of six voices, zero reported stream
underruns and console RX drops, and zero voices after Stop. Evidence:
`logs/melodic-soak.log/.xml`, `logs/hil-20260920-181630.log` and
`logs/melodic-soak-summary.json`. Parent-grid and Notes navigation were also
exercised during the loop. No physical MIDI, listening, audio recording or
DWT deadline proof is inferred from this resident-sample soak. UART TX-pressure
warnings occurred in the combined session; zero console RX drops does not
mean lossless inter-MCU telemetry.

**Half-step/feedback completion:** the final-source melodic HIL passed in
20.64 s (`logs/melodic-halfstep-hil2.log/.xml`) on profile images Daisy
`a462346519a7783734f854f63e1dbc1b18ccdfa6445bf96a33f128e926a99730` and ESP32
`74ceed7265be9f51b5b70eae9cb1285a21ada9dc5d03f60106175acb3728432f`.
It verifies the half-step readback/control cycle and transient confirmed-step
feedback in addition to the earlier melodic workflow. The first attempt
incorrectly toggled Shift again after a shifted softkey automatically cleared
it; the harness was corrected, with no firmware change for that failure.
The final source passes 457 shared, 808 Daisy and 367 ESP32 host tests, both
normal builds and the alternate Stage B compile check. Scale-constrained entry
still depends on the Phase 5 tuning/mask model; current entry is chromatic.

**Final normal images:** all **11 hardware regressions passed in 92.56 s**,
including half-step controls and feedback (`logs/melodic-final2-hil.log/.xml`).
Daisy SHA256 `c344a6699144bdfa290e997636281ffd2d0da6862d8ea3ab36f17ecd5ebc21c7`;
ESP32 `0b71e8aae8037f214ec785c20ebcc65a6b3d5d3e0ea6cc8ab2a45659f636027b`.
The final-source eight-note DWT screen averaged 44.1541%, peaked at 77.1698%
and leaves capacity deferred; see the [complete pressure evidence](callback-performance-log.md#melodic-chord-pressure--2026-09-20).
The boards were left on normal firmware at Main Menu with an empty bench RAM
session, zero voices/streams/samples, zero reported stream underruns/console
drops, and profiling disabled. Final readback: `logs/melodic-final-state.json`.
Source WAVs and existing saved assets were preserved; tests used unique save
copies. The final-source physical recording/audio and extended capacity gates
remain open; the earlier musical soak is not silently reassigned to new images.

## HV-025 — Standalone sample saves

**Status:** partial; automated two-board save/recall checks passed. Listening,
power-loss recovery and loaded audio timing remain open.
**Scope:** [Phase 1.5](roadmap.md#phase-15--sample-editing) and the
[standalone save model](features/offline-sample-editing.md#standalone-edits-as-built).

**Setup:** both updated boards, serial logs, audio monitoring, a disposable or
backed-up FAT card with a multi-minute PCM16 WAV that fits available resident
RAM and short mono/stereo fixtures. Record both image hashes. Use unique copy
names; recovery tests require a disposable copy, never the only source asset.

- [ ] **025a — Save/reboot/reload:** load the long WAV, edit start/end/loop/gain
  and fades, Shift → Save → Save edits. Confirm completion, reboot both boards,
  reload and audition. Metadata and audible region match; UI remains responsive.
- [ ] **025b — Save As:** save a unique new basename. During the copy navigate
  and query status. Confirm unchanged source WAV hash and byte-identical copied
  WAV, independent sidecar, unchanged Track bindings and no automatic reload.
  Reusing the name fails without changing either file.
- [ ] **025c — Ownership:** compare fresh Instrument dependency import and
  standalone audition with saved defaults. Save a Project, alter the standalone
  sidecar, recall the Project and verify its captured edits still win.
- [ ] **025d — Failure/recovery:** use a disposable card/copy to interrupt each
  publish stage and simulate full/removal cases. Reboot and verify valid old/new
  edits or an explicit failure; no false success, source loss or silent reset.
  Record orphan files and recovery steps; FAT power-loss behavior remains open
  until this passes on the tested card/filesystem.
- [ ] **025e — Link loss and audio:** drop/restart each peer during Save As,
  restore the link and verify no duplicate copy request. An unknown outcome
  stays unconfirmed. Under resident playback verify responsive Stop/note-off,
  no added stream underruns and the current callback timing limits.

**2026-09-20 bench result (UTC logs dated 2026-09-21):** final normal QSPI
images pass all five Sample Edit HIL tests in **64.84 s** and the standalone
save/copy/reload test using `/03 Lips of Ashes.wav` in **202.41 s**. This is
12,312,576 stereo frames at 44.1 kHz (4 min 39 s, 49,250,304 PCM bytes).
The long-file test copies the entire WAV under a unique name, rejects a second
copy to that name, reloads the saved markers/gain, changes them, saves the
sidecar and verifies a second fresh Pool load. It checks unchanged Track
bindings during Save As and continuously polls UI state during the copy.
Source WAVs and existing sidecars are preserved; unique `HIL Edit ...` copies
remain on the card. Physical byte hashes were not measured; byte-for-byte copy
is covered by the host storage test.

A separate software restart of both images restores
`/HIL Edit 20260921 015309.wav` through **nonresident streaming** with start/end
256/4096, loop 512/2048, gain Q15 11626 (−9 dB), zero Pool records and repeated
loop rewinds. The stream reports zero underruns. This verifies persisted
standalone defaults without a surviving RAM record; it does not simulate a
power cut or certify audio quality. The final long-file test's copy is
`/HIL Edit 20260921 020049.wav`.

Evidence: `logs/sample-edit-hil.log/.xml`,
`logs/sample-edit-long-hil.log/.xml`, `logs/sample-save-reboot-result.json`,
`logs/sample-save-reboot-console.log`, and the real-LVGL layout capture
`logs/sample-save-as.png`. Early bench attempts exposed test setup races:
overlapping browse requests, too-short resident-load timeouts, stale retained
save completion and setup cleanup. The HIL helpers now serialize browsing,
wait for a changed completion ID and clean up after setup failure. Those
attempts are not counted as passing firmware checks.

Final images from base `7fbee5ed21d189ae0e762f27db5b86de6630712f` plus this
uncommitted sample-save change:

- Daisy SHA256 `d80648569de03caf65e041182aab3727cb953c183aeba6183a565e39478db0f9`.
- ESP32 SHA256 `11b082ec034abd8b166b53e854f44333471eb1498adf82acda4c8bb6646f7ab1`.

All pre-commit checks pass, including both firmware builds and **462 shared,
815 Daisy and 371 ESP32 tests** (`logs/sample-edit-approved-checks.log`).
The audio callback was not changed; this run adds no DWT capacity result and
does not close any existing soak/callback gate. Project sidecar precedence,
short writes, close/rename failures and backup fallback were host-tested;
physical fault injection and listening remain unrun, so the full boxes above
stay open.

**Remaining Phase 1.5 blockers:** stereo snap/seam and crossfade acceptance
(now tracked in HV-026), and complete
stream/RAM channel and gain parity. These are not covered by a sidecar pass.
File-backed editing was removed from scope on 2026-09-21; editing requires a
complete resident sample.

### SD write-clock comparison, 2026-09-21

The inserted 60,906 MiB card reads the 49,250,304-byte PCM payload in
`/03 Lips of Ashes.wav` at about five seconds at 25 MHz / 4-bit, but a unique
Save As failed after **1,155,072 copied bytes**, FatFS output error 1 and
HAL SD error `0x00000002` (data CRC). A matched 12.5 MHz / 4-bit image passed
the complete unique-copy Save As, pool unload/reload, sidecar edit-save and
second reload check in **82.47 seconds**. Original WAV and sidecar untouched;
no formatting was performed. Evidence: `logs/sample-save-125-hil.log`,
`logs/sample-save-25-hil.log`, their flash/build logs and the `SAMPLE_FILE`
line in `logs/daisy.log`.

- Diagnostic 12.5 MHz Daisy SHA256:
  `90611c3214c0b1b6b792bd5baf7bd7c25976616dbb266d3aa42c90fd448b68f8`.
- Diagnostic 25 MHz Daisy SHA256:
  `9ea7c9bed4a954448144da74ea58dfb95b3bfac762eb0e8fb76b55de4b19385e`.
- ESP32 SHA256: `36aa058a2cfbe77da5a922e93de1902766a51706ed7f94a711976319b4a6f93c`.

The implementation now downshifts after a sample-file job reports an SD
peripheral I/O failure and has closed its handles. It retains 4-bit mode and
the failed operation result; retry is an explicit user action. Boot/insertion
still starts at 25 MHz. A read-only mount does not certify write reliability.
The final-image retry test failed: a 25 MHz write CRC after 4,923,392 copied
bytes triggered the 12.5 MHz attempt, but `HAL_SD_Init` returned command timeout
`0x00000004`; subsequent browse was unavailable, so explicit retry could not
start. Read load immediately before this failure took **5.172 seconds**.
Evidence: `logs/sample-save-auto-first.log`, `logs/sample-save-auto-retry.log`
and `logs/daisy.log`. Images match HV-028 below. Automatic write recovery remains
**failed/open**; a successful fixed-clock run does not establish recovery after
a bus fault. Physical power-loss/listening and whole-phase acceptance remain open.

## HV-026 — Stereo snap, seams and crossfade

**Status:** Partial; software checks pass, physical acceptance remains open. **Source:**
[Phase 1.5](roadmap.md#phase-15--sample-editing),
[stereo edit semantics](features/offline-sample-editing.md#stereo-markers-seam-checks-and-playback-crossfade).

**Setup:** matching protocol-7 images, stereo monitoring/capture, normal SD
card, a stereo PCM16 source with independent L/R phase and an anti-phase/DC
fixture; mono, 44.1/48 kHz and PCM24 fixtures for streaming. Use unique Save As
copies. Keep profiling disabled in the final functional images.

- [ ] **026a — Panel and snap:** Select each marker with touch and encoder,
  invoke Shift → Snap, and inspect raw stacked L/R seam halves. Both native
  channels must cross at the accepted frame; a no-crossing result leaves the
  marker unchanged. Moves stay within the search radius, trim and loop bounds.
  Move a marker while a deliberately delayed request is in flight; stale
  expected edits must not overwrite newer state. Repeated entry/exit must not
  replay requests or retain waveform listeners. Readout and action labels fit.
- [ ] **026b — Audition and note audio:** On a copy, try crossfade off, 1 ms,
  20 ms and a loop short enough to clamp the effective overlap. Capture repeated
  streamed auditions and resident notes at unity and fractional pitches. Verify
  linked L/R weights, no out-of-region reads, the documented shorter period,
  matching transition shape and no unexplained click/dropout. Check opposite
  phase and correlated material; check secondary oscillators and Mono mode.
- [ ] **026c — Persistence:** Save and Save As, unload and reload; repeat after
  software restart and power cycle. Nonresident streaming and Project recall
  must retain crossfade, with Project snapshots overriding external sidecars.
  Older sidecar 1.0/Project 1.3 files must load with overlap off.
- [ ] **026d — Callback budget:** Capture before/after DWT with full channel
  reservations, two oscillators, filters, modulation, locks, note bursts,
  sequencing and SD audition. Include short loops with the maximum legal
  overlap, mono and stereo. Each accepted workload needs at least ten minutes,
  zero stream underruns and peak callback below 70%; 70–80% requires profiling
  and >=80% requires backend-upgrade planning. Preserve binary hashes and logs.
  The full mixed-channel one-hour gate remains HV-019.

Listening, physical touch/encoder, PCM capture and power-cycle checks remain
unrun until explicit evidence is recorded. Host and console tests do not
replace these checks.

**2026-09-21 UTC — software validation and interrupted timing comparison:**

Base `7fbee5ed21d189ae0e762f27db5b86de6630712f` plus uncommitted sample-save
and stereo seam/crossfade changes. Container builds pass for ESP32, normal
Stage A Daisy and alternate Stage B Daisy. All **471 shared, 818 Daisy and
375 ESP32 tests** pass. Coverage includes stereo zero-crossing rejection,
bounded linked-channel mixing, fractional-pitch resident PCM against a baked
reference, streaming chunk boundaries, region fades, stale request handling,
real LVGL page lifecycle and old/new sidecar/Project formats.

Before the callback change, the full eight-channel/four-stereo-voice workload
ran 606.2 seconds (121 windows), with two oscillators, ladder filtering,
modulation, locks, 16-Track note bursts, sequencing, SD audition and Pattern
save/load. The one-second loop baseline averaged 28.9% and peaked at
**320,513 cycles / 66.7735%**, with zero stream underruns. It passes only that
baseline workload. Evidence: `logs/stereo-4-ladder-20260921-023156.log/.json`;
see [callback performance log](callback-performance-log.md).

The subsequent 512-frame stereo baseline stopped around 541 seconds after
unexpected page navigation (`logs/stereo-4-ladder-20260921-024417.log/.json`).
The Mono baseline failed setup after unsolicited marker and Stop/Play events
changed the workload (`logs/stereo-0-ladder-20260921-025723.json`). Neither is
a qualifying timing run. Controlled measurements were paused; their cause
was not established. **No candidate DWT result or performance improvement is
claimed, and 026d remains open.**

Baseline profiling SHA256:
`4adb9f177a1048ccbc66ac9d0c3f22ca995ffe2ba05b0f84b2724f2f820d1ab4`.
Final candidate profiling binary, built but not measured:
`a4b9981c1d14f86d36053731e00c9cecca311ba022bd06edf91056a7e3f08ea0`.

Matching normal images were flashed with Daisy profiling disabled:

- Daisy SHA256 `dede6386b1c5e710afefc1633d25f64bec49f9be8c49ecdfc44280120476f4bd`.
- ESP32 SHA256 `3ffa6ffe93eba3fb4fbb54e3d37776b7ce8c80d9a972987ebdf8c79b4ca718a2`.

All pre-commit checks pass (`logs/sample-seam-final-approved-checks.log`). On
these images, the six short-fixture two-board Sample Edit tests pass in 73.58 s
(`logs/sample-seam-hil.log`, `logs/hil-20260921-030802.log`). This includes
crossfade Save As at 13 ms and subsequent Save at 5 ms with unload/reload,
collision rejection, correlated seam/snap controls at 20 ms, audition rewinds,
resident held-note playback, unchanged unrelated streams and repeated page
exit during waveform traffic. The default kick fixtures are mono; this run
does not establish native stereo audio quality.

The Save/Save As and seam-control cases also pass with the 49,250,304-byte
native-stereo `/03 Lips of Ashes.wav` fixture: **2 passed in 244.99 s**
(`logs/sample-seam-stereo-hil.log`, `logs/hil-20260921-030929.log`). The copy
is `/HIL Edit 20260921 031008.wav`; the source WAV was not modified. Reload
restores 13 ms and subsequently 5 ms, while the control case checks 20 ms,
bounded snapping, audition rewinds and resident note lifetime. The capture
contains seven UART TX-queue pressure warnings; these passing functional
checks do not certify transport capacity or audible stereo seam quality.

Real-panel framebuffer captures on stereo `kickatb.wav` were inspected:
`logs/sample-seam-panel.png` and `logs/sample-seam-panel-shift.png`. Stacked
raw L/R seam halves, the 20 ms tile, effective 768-frame overlap, separate
L/R source-jump readouts and shifted Check Seam action are visible without
clipping. This verifies rendering, not physical finger/encoder behavior.

**2026-09-21 UTC — 026a input regression follow-up:** The user's report of
inert drags reproduced on the tile fill/knob: their decorative LVGL objects
were separately clickable and intercepted pointer events. Loop-handle focus
also hid the held handle by switching to the seam view. The page ignored the
navigation encoder's `ButtonPress` event and did not select tiles on touch.
Five new real-LVGL regressions cover these paths and stable tile positions.
The fix makes decoration transparent to hit-testing, keeps the continuous
view while any marker is held, selects touched tiles and accepts navigation
encoder pushes. Physical encoder behavior remains deferred until the user's
two-encoder protoboard wiring is ready; injected events do not validate wiring.

The **380 ESP32 unit/widget tests** and **7 two-board Sample Edit tests** pass.
The new board case touches each tile, drags every fill bar, moves each of the
four held handles twice, verifies backend readback, and exercises encoder-push
selection (`logs/sample-input-hil.log`, `logs/hil-20260921-034002.log`). The
remaining sample-edit/persistence cases pass too; zero sampled stream
underruns. The marker input case also passes on the multi-minute stereo WAV
in **44.66 s**, including its load and all eight drag targets
(`logs/sample-input-long-hil.log`, `logs/hil-20260921-034411.log`). This is
eight passing board cases across the two fixture runs. All pre-commit checks
pass (`logs/sample-input-approved-checks.log`). ESP32 SHA256 is now
`a4dbb2b269ad490660609799098dc9e2935ff5a50bf06e68696ebb66b78b644b`;
Daisy is unchanged from the normal image above. The new ESP32 image is flashed.

The initial comparison used two captures already running the slow fallback:
49,250,304 PCM bytes read in **32.966 s / 1493 KB/s**, versus **33.002 s /
1492 KB/s** in the earlier pre-sample-save/crossfade
`logs/polyphony-card-daisy.log`. Current end-to-end Load-to-ready was
**33.365 s**, and switching to Edit took **0.224 s**; browsing from the prior
directory to the selected root entry took **1.954 s**. Evidence:
`logs/sample-load-timing-check.log` and
`logs/sample-load-timing-transcript.log`. Both captures report the existing
12.5 MHz, one-bit SD configuration; no storage clock or read path was changed.
This comparison covers that fixture and setup, not every card/directory.
**Correction after the user's five-second recollection:** seven September 16
captures load the same 49,250,304 PCM bytes in **5.160–5.161 s**. For example,
`logs/hil-20260916-052607.log` reports **9544 KB/s** and STANDARD/25 MHz.
The previous wider bus was replaced by the narrow, slower defaults during the
September 18 write-failure investigation documented in HV-001. Thus the load
slowdown is real (about 6.4 times), and predates sample-save/crossfade; the
initial conclusion that it did not reproduce missed that earlier baseline.
The current boot starts at the conservative setting and mounts there, rather
than trying the old faster setting first. Restore faster/wider operation only
with read and write validation against the unresolved HV-001 failure.
An initial timing script stopped at an occupied-Track replacement prompt; it
was interrupted, corrected to use an empty Track, and excluded from timings.

## HV-027 — Sample playback channel selection

- [ ] **Gain parity regression (2026-09-22):** Use a quiet reference tone,
  compare streamed audition with unity-velocity resident playback at -24,
  -12, 0, +6 and +12 dB, then repeat with a full-scale signal. Expect matching
  gain before output clipping, no mute at -24 dB, and saturation without wrap
  at positive gain. Measure refill duration and callback DWT peaks before/after
  the foreground CMSIS scale change. Physical listening/timing remain unrun.

**Introduced:** 2026-09-21. **Gate:** [Phase 1.5](roadmap.md#phase-15--sample-editing).
**Design:** [playback channels](features/offline-sample-editing.md#playback-channels).
**Setup:** matching protocol-8 images; asymmetric stereo PCM16 sample and a mono
sample; both serial loggers, panel/audio capture and Daisy DWT profiling.

- [ ] **027a — Channel/control mapping:** Load stereo, open Edit and select
  CHANNEL using touch and the navigation encoder. Cycle Recorded, Left, Right,
  Mono Sum and back. Confirm matching metadata, waveform revision and lane
  labels after each change; exit/re-enter and repeat while auditioning. Check
  mono source audition on both outputs. **Pass:** channel identity agrees with
  source PCM, labels and output; no stale waveform or stuck audition.
- [ ] **027b — Resident ownership:** Trigger held stereo notes, change sample
  mode, then retrigger. Check both oscillators, oscillator Mono, sequencer/live
  admission and fractional-pitch crossfaded loops. **Pass:** old notes keep
  their selection; new notes use the confirmed mode; channel reservations
  match mono/stereo results without expanding the configured budget.
- [ ] **027c — Persistence:** On write-validated media, Save As to a unique name,
  Save changed modes, unload/reload, power-cycle and recall a Project override.
  **Pass:** saved modes and Project authority survive; original PCM remains
  intact. **Blocker:** current four-bit Save As failure recorded in HV-001.
- [ ] **027d — Timing/soak:** Compare before/after DWT at full channel capacity
  with both oscillators, filters, locks, modulation, streaming and file jobs.
  Exercise local tile adjustments and idle redraws with UI profiling. Apply
  HV-019/HV-026 thresholds; short diagnostic screens do not close the soak gate.

**2026-09-21 automated results:** 473 shared, 819 Daisy and 381 ESP32 host
checks pass (1,673 total), including channel polarity/sum limits, selected
channel reservation counts, fractional-pitch crossfades, complete edit/seam
wire snapshots, Project waveform-revision invalidation and real-LVGL controls.
ESP32, normal Daisy and alternate Stage B builds pass.

The new two-board stereo-channel case passes in **15.27 s** on Daisy profiling
image `081cbdb317abba2740fe2d8a28dc7609245bac4577763f702a9d0cd7e59d9be0`
and ESP32 `24cf4077f5a8e78db8aa60050340530a492d060910b6ba443b22f58e43dd0f66`.
It cycles all modes through the editor's injected encoder event, confirms
backend metadata/revision and stereo output meters, and verifies eight mono
notes versus four stereo notes at the fixed channel budget. Evidence:
`logs/sample-channel-hil.log` and `logs/hil-20260921-044437.log`. This does not
validate physical encoder wiring or listening.

The isolated 40-second [DWT screens](callback-performance-log.md#sample-channel-selection--diagnostic-screen-2026-09-21)
show 61.2458% baseline / 60.7585% candidate peak, with zero sampled underruns
or dropped events. Average cost increases from 39.9541% to 42.9408%. Different
SD settings limit comparison; no performance improvement is claimed. Both
screens stop before any file cycle, so neither closes the full timing gate.
The initial candidate setup with a cumulative dropped count of one was
excluded and followed by a fresh reset. Write recovery remains blocked by
HV-001. Physical touch, listening, power-cycle recall, UI profiling and extended
mixed-channel/file-operation soaks remain unrun.

**Final normal-image checks:** six non-writing Sample Edit regressions pass
in **70.35 s** (`logs/sample-channel-normal-hil.log`,
`logs/hil-20260921-045030.log`). Standalone Save/Save As was deliberately
excluded because HV-001 already records the current write failure. The final
framebuffer capture `logs/sample-channel-mono-sum.png` shows the CHANNEL tile,
a single summed waveform and a legible `(L+R)/2` label. The initial capture
showed poor label contrast over PCM; a compact opaque label background fixes
it, followed by another ESP32 build, all 381 frontend tests and recapture.
This is framebuffer inspection, not physical-panel or touch-latency evidence.

Final flashed normal images, profiling disabled:

- Daisy SHA-256 `fddfee625d5bc294236cb7722bcf49f58b519a2fa5a647d53c4c1f94e6d9dd57`.
- ESP32 SHA-256 `36aa058a2cfbe77da5a922e93de1902766a51706ed7f94a711976319b4a6f93c`.

Build/host evidence: `logs/sample-channel-{host-tests,final-checks,stageb-build}.log`;
formatting fixes rechecked in `logs/sample-channel-final-lint.log` and the
final label change in `logs/sample-channel-label-checks.log`. Screenshot
transcript: `logs/sample-channel-screen-final.log`. No firmware release bump
or phase-gate closure is implied.

## HV-028 — MIDI expression

**Status:** Partial; host and console-injected checks pass, physical acceptance open. **Source:**
[Phase 2.5](roadmap.md#phase-25--sampler-instrument-layer),
[MIDI modulation ownership](features/param-locks-and-modulation.md#midi-source-ownership-as-built).

**Setup:** matching current debug images, resident mono/stereo loops, a DIN/USB
controller with mod wheel and channel pressure, stereo monitoring/capture.
Physical DIN wiring is still a prerequisite. Use a profiling image only for
DWT capture; restore profiling-disabled images afterwards.

- [ ] In Instrument Mod, choose Mod Wheel → Pan and Pressure → Cutoff. Change
  each source on a held note and listen for the assigned change; zero returns
  to the unmodulated base. Verify Apply/Revert and a uniquely named WXI recall.
- [ ] Set separate Tracks to MIDI 1, MIDI 2, Omni and Off. Each physical port
  reaches only matching/Omni Tracks; Off ignores expression. Held/releasing
  and newly triggered notes use the same Track values. DIN/USB share channels.
- [ ] Send CC121; both supported sources return to zero on matching Tracks.
  Change routing or replace a Track's Instrument; its old expression clears.
  Reboot starts at zero. A removed port does not invent a reset message.
- [ ] Measure MIDI-to-audio latency and DWT under the full voice/modulation,
  sequencer, streaming and file-operation workload, then complete the one-hour
  zero-underrun soak. Controller floods must not block notes or clock output.

**2026-09-21 software and board evidence:** 475 shared, 822 Daisy and 382 ESP32
host tests pass; both MCU builds and Stage B compile. The two-board expression
check passes in 9.35 seconds on normal images, exercising frontend byte parser,
wire forwarding, UI source selection, matching/nonmatching channels, Omni,
Off, CC121 and routing resets against codec-bound stereo meters. A first
attempt used a drum one-shot that expired during observation; the passing
fixture uses a sustained looping keyboard Instrument. These are digital meter
observations, not external audio listening or physical port validation.

Evidence: `logs/midi-expression-normal-hil.log`;
[matched and active-expression DWT screens](callback-performance-log.md#midi-expression--diagnostic-screen-2026-09-21).
Normal Daisy SHA256 `24c854d762624c341a9b6914d1202643fb8cc7590fb51de56bbc08656b991918`;
ESP32 SHA256 `99240dc27371d6c099ea0013a7c12cb62620c6de30f203ce4468da963f4c92ac`.
Profiling is disabled. No physical checkbox is closed by these checks.

Host/parser/forwarder and console-injected checks are partial evidence only;
they do not establish physical MIDI timing or listening acceptance.

## HV-029 — Expanded modulation and live locks

**Introduced:** working changes after `90b7329`, 2026-09-21.
**Gate:** [Phase 2.5](roadmap.md#phase-25--sampler-instrument-layer).
**Behavior:** [modulation and live lock capture](features/param-locks-and-modulation.md).
**Setup:** matching images, two distinguishable resident oscillator samples,
MIDI wheel/channel pressure, panel controls, audio capture/listening and DWT logs.

- [ ] Route wheel to Osc mix; sweep both extremes including a base mix of zero.
  Confirm independent levels, zone gain and an OSC2-only primary source remain
  correct. Clear routes and Apply/Revert without a retrigger or stuck mix.
- [ ] Route envelopes/LFOs to each LFO rate. Check Hz and Sync, ±4-octave depth,
  0.01–100 Hz limits, phase continuity and stable self/cross modulation. Save an
  Instrument and power-cycle/reload; settings and audible movement must return.
- [ ] Step Notes → Live rec → Play, then move Play-page cutoff/resonance/ADSR.
  Inspect Locks for the armed Track: repeated parameters replace their slot;
  the fifth distinct parameter evicts the first slot. Other Tracks, stopped
  transport, Song playback and Play/Step/Erase modes must not capture motion.
- [ ] Play back captured locks and compare with live motion. Save Pattern/Project,
  power-cycle/reload and verify the overrides. Confirm that note gates, swing,
  note quantization and a queued Pattern replacement remain correct.
- [ ] Repeat the full callback workload, file operations and one-hour mixed-channel
  soak on final images. Meet the unchanged capacity threshold and zero-underrun
  requirements; short diagnostic runs do not pass this gate.

**Partial evidence, 2026-09-21:** protocol destination/persistence tests and
Daisy mapping, phase/lifetime, lock timing/ownership tests pass. The two-board
`tests/hil/test_live_locks.py` check passed (9.74 s): captured five controls,
confirmed UI feedback, four-slot eviction, wrong-Track/stopped protection and
no added notes. Evidence: `logs/live-lock-hil.log`. This is console-injected
control traffic, not physical knob or MIDI latency/listening evidence.

**Final normal-image recheck:** both new HIL cases passed (15.93 s), covering
live capture/readback and selection/preview/Apply/Revert of all three added
modulation destinations. Evidence: `logs/modulation-ui-final-hil.log`.
The complete container pre-commit suite passed both firmware builds and shared,
ESP32 and Daisy host tests (476, 382 and 832 respectively). Stage B also compiled;
it was not flashed. The subsequent header layout fix passes all 382 frontend
tests and the ESP32 build (`logs/ui-header-checks-final.log`).

HIL profiling-disabled image SHA256:
Daisy `b01d214d3621a5f1ae05b667f4d385c7dcb218468b9ae543d87c2aa13de801bd`;
ESP32 `b01c43e9b162dcf58e01cadecd0004155f51accdea989d27b0510779aa246ecd`.
The final Songs-only disabled-style correction also passes the ESP32 build and
all 382 frontend tests (`logs/ui-song-contrast-final-checks.log`). Final flashed
ESP32 image: `f56a1b535240b5c370e694d9f1e18e16db488f01bd091223ef3cea323f39e91e`.
Daisy is unchanged; this UI-only correction does not repeat the feature HIL.
The sparse-publication DWT image was
`1da8f16001f04a5e578f3ba32acec67b8d16aef2a6518aff22d333981af442c4`.

DWT images, workload details and the over-threshold live-lock result are in
[the callback report](callback-performance-log.md). Extended timing, physical
controls, listening and save/reboot acceptance remain open.

## HV-030 — Codec and internal-mix recording

- [ ] **030a — Direct assignment:** after a successful codec/internal Save,
  use Shift → To pad / To keys, select an empty and an occupied destination,
  then Assign take. Confirm the same Pool sample is assigned, existing ranges
  and sound settings survive, and other Tracks keep playing. Cancel/back must
  leave maps untouched; unsaved takes disable the shortcut. Disconnect during
  Done must not navigate or replay the action after reconnect. Save/reload the
  resulting Instrument and listen. Host acknowledgment tests and frontend
  build cover the implementation; this physical check remains open because
  the card write CRC currently prevents reliable save/reload acceptance.


**Design:** [Sampling and recording](features/sampling-and-recording.md).
**Status:** partial; implementation continuation authorized on 2026-09-21.

Setup: matched normal images, writable card, a stereo codec-input test signal and
resident sequenced material for internal resampling. Record tested image hashes.

- [ ] For codec stereo, left, right and internal mix: arm manually and by threshold;
  verify pre-roll chronology, selected-source RMS/peak/clip feedback, maximum
  length and input monitoring without internal feedback.
- [ ] Stop, audition within 100 ms, save under a unique name, Done, assign to an
  Instrument/Track, sequence, reboot and reload. Verify stereo content and markers.
- [ ] Inject full/failed media, allocation refusal, ring overflow, stale take IDs,
  reconnect and delayed callback acknowledgements. Preserve the contiguous RAM
  take on failure; no replayed mutation or freed sounding buffer.
- [ ] Measure capture/save with eight Mono and four stereo voices, both oscillators,
  modulation/locks and streaming. Complete capacity and zero-underrun soak gates.

2026-09-21 partial: host capture, writer, ownership, protocol and UI checks pass.
Both codec-stereo and internal-source board flows pass capture, audition, Save,
Done, Track assignment and unload (`logs/recording-shorttemp-hil.log`, two tests).
Long temporary names produced `FR_NO_FILE` at sidecar publication at both 25 and
12.5 MHz; short transaction names passed with normal clock negotiation restored.
A real-FatFs RAM-disk reproduction passed with long names, so the precise target
cause is not established. A subsequent matched-image retry hit `FR_DISK_ERR`
during WAV data write at 25 MHz (`logs/recording-task3-hil.log` and
`logs/recording-task3-retry-hil.log`); the RAM take remained available.
Full-workload short screens captured 960,000 frames per source: internal mix
with eight Mono voices (peak 9,299, no clipped frames) and codec stereo with
four stereo voices (background input peak 9). Both had zero capture errors and
sampled stream underruns, plus one Pattern file cycle after discarding the take.
A 90-second take was correctly refused for insufficient resident memory.
See [DWT evidence](callback-performance-log.md#recording-arpeggiator-and-performance-controls--2026-09-21).
Physical input signal/content, listening, reboot recovery, latency, loaded WAV
saving and soak remain open. Final image evidence follows below.

## HV-031 — Arpeggiator

**Design:** [Arpeggiator](features/arpeggiator.md).
**Status:** partial; host and selected board checks do not close physical acceptance.

Setup: matched images, a gated keyboard Instrument, DIN/USB MIDI chord input and a
DAW clock source; enable the Instrument Arp panel.

- [ ] After the 2026-09-22 publication fix, enable Arp from its default-off
  state, change every setting and disable it again while holding a chord.
  Verify audible generated timing follows the published settings, including
  rapid edits; capture before/after callback DWT peaks on the matched workload.

- [ ] Hear every mode, octave range, division, gate, velocity and latch behavior;
  repeated pitches and stale releases must preserve the current chord/group.
- [ ] Verify Apply/Revert and Instrument/Bank/Project save/reload, rebind, disable,
  Stop/Continue, reconnect and live recording of generated notes.
- [ ] Run ten minutes against MIDI slave clock without grid drift; measure physical
  note latency and full callback workload, then complete the one-hour soak.

2026-09-21 host evidence: golden modes/latch/identity and ten-minute exact sample
clock tests pass (`logs/arp-core-tests.log`, `logs/arp-runtime-tests.log`).

2026-09-21 board evidence: Instrument Arp edits, shared Apply/Revert and generated
voice admission pass (`logs/arpeggiator-hil.log`, one test). Physical MIDI,
listening, generated-note recording and persistence combinations remain open.

## HV-032 — Global LFO, held locks and diagnostics

**Design:** [Modulation](features/param-locks-and-modulation.md) and
[UI architecture](ui-architecture.md).
**Status:** partial; selected console-driven checks do not close physical acceptance.

Setup: matched images, a modulated Instrument, wired encoders, MIDI input and a
one-step Pattern. Preserve image hashes and console/render logs.

- [ ] Exercise global waveform/Hz/sync/restart/reset; navigate away and back,
  Apply/Revert/Save an Instrument, and confirm the session settings stay separate.
  Listen to free/transport/note restart and MIDI-tempo following.
- [ ] Short-tap and hold steps; turn each encoder while held. No hold toggles a
  note. Release/lost touch, Track change, navigation, reconnect and Pattern
  replacement cancel the hold and reject stale edits. Check physical feel/latency.
- [ ] Record five distinct controls into one step. Hear/inspect four resulting
  locks and see the named eviction notice on Play and Sequencer; repeated updates
  to one parameter do not report eviction. Check a burst and return to normal header.
- [ ] Send known MIDI note/CC/clock counts per interval; compare diagnostics with
  the source, excluding UI/generated notes. Reopen diagnostics without boot-count
  spikes and disconnect without stale values. Verify clock/transport readback.
- [ ] Capture changed pages and measure entry/idle/edit rendering; repeat the
  callback workload and retain all capacity and soak requirements.

2026-09-21 board evidence: four tests pass for global-LFO edit/reset/re-entry,
held-step editing without toggling and cancellation on Track change, actual
received MIDI counters, and fifth-lock eviction notice/readback
(`logs/task3-controls-hil.log`). Console injection does not establish panel or
physical MIDI behavior. Image and render evidence is recorded below.

### Recording and controls image provenance — 2026-09-21

Final normal, profiling-disabled images over `90b7329+`:

- Daisy SHA256 `bb4502317c87549b523f402e60c5d7f0017869d3218b4b0337a8da73fd00f822`.
- ESP32 SHA256 `af3073b1f3b0202962c1b131f737a9ce25d3fb84f57a5565a7e6c4a6b4076510`.

484 shared, 863 Daisy and 388 ESP32 host tests pass; normal Stage A and alternate
Stage B output/CV configurations compile. These are software checks, not whole
phase gates. DWT and rendering use separate explicitly identified profile images.

Normal-image HIL used ESP32
`46202818710dd705337bd27b2b08a01d7cc8f8ab8e3af31a4bcb6c13060c2b52`
before the final unit-label/recorder-help text cleanup. Five controls/arpeggiator
tests pass; both recorder
workflows capture and audition, then fail Save with `FR_DISK_ERR` (first at WAV
payload write (phase 3), then directory setup). RAM take retention was confirmed and the
bench take discarded afterward. Evidence: `logs/task3-final-hil.log`,
`logs/hil-20260921-182530.log`. These failures keep HV-030 Save/recovery open.

[Render measurements](ui-latency-notes.md#recording-and-performance-controls--2026-09-21)
confirm partial individual edits and retain the 243.52 ms Locks-entry redraw
as open follow-up. All 68 native gallery views were refreshed and their
1280×800 PNG dimensions verified: `logs/ui-pages-20260921/index.html` and
`logs/ui-pages-20260921.zip`. Final-image global-LFO edit/reset/re-entry passes
(`logs/task3-final-global-hil.log`, one test); screenshots verify the corrected
Global LFO unit labels and state-specific recording help.

## HV-033 — Instrument tags and filtering

**Status:** Pending; host tests and both firmware builds only (2026-09-21).
**Behavior:** [Instrument tags](features/track-and-patch-model.md#34-tags).
**Setup:** matched protocol-9 images, several WXI sounds with overlapping tags,
SFZ imports, folders and more than one page of Instrument entries. Record image
identities. SD save/reboot checks remain blocked by the unresolved write CRCs.

- [ ] Stage multiple categories, Revert, then Apply while notes sound. Confirm
  tags change only on Apply and sounding voices continue without interruption.
- [ ] Save a new Instrument copy, reload it and reboot; categories must persist.
- [ ] Cycle every browser filter; only matching WXI and folders remain, All
  includes SFZ, and returning to the page preserves the chosen filter.
- [ ] Browse beyond one page, select/load a matching sound and enter/leave a
  folder; labels, selection, paging and loaded identity must agree. Account for
  the documented 256-entry prefilter cap.
- [ ] Change Track, replace its Instrument, disconnect/reconnect the peer and
  delay replies while editing; stale edits must not overwrite the new sound.
- [ ] Interrupt metadata reads with card loss, malformed WXI and a busy storage
  job. No crash or audio underrun; filtering resumes or can be refreshed after
  recovery, without loading sample PCM just to classify a sound.

## HV-034 — Frontend note, browser and load recovery

**Status:** Open; host regressions and builds do not establish physical acceptance.
**Design:** [frontend remediation](roadmap.md#esp32-frontend-audit--2026-09-22),
[UI ownership](ui-architecture.md#cross-task-updates), Phase 2.
**Setup:** paired current images, sustained Instrument on two Tracks, DIN/USB
MIDI source, SD folders exceeding 20 entries, two resident samples and UART
fault/queue-pressure instrumentation. Record image identities and transport logs.

- [ ] F1/F2: hold/latch notes, switch Tracks, transpose, change tabs and leave
  Play while producing MIDI bursts and rejected TX admissions. After recovery,
  every accepted note-on releases on its original address; All Off releases
  pending notes and rejected note-ons never replay.
- [ ] F3/F5: rapidly change folders/filter/page while delaying directory replies,
  then leave/re-enter Browse. No stale labels, incorrect paths, crash or mixed
  metadata; old replies cannot satisfy a new request. Repeat with card removal.
- [ ] F4/F7: select resident A, fail a new load by enqueue/SD/RAM rejection and
  open Edit. A remains selected. Reuse a resident file larger than the remaining
  free block on another Track; identity is shared and allocation does not grow.
- [ ] F6: give two Tracks different filter/envelope values, enter/re-enter Play,
  switch Tracks and interrupt readback. First adjustments start from confirmed
  values; stale/disconnected values cannot authorize edits or cause jumps.
- [ ] F8: reject the bind send after successful loading and delay binding replies.
  Do not report success until matching readback; recover while the page remains
  active without another load or duplicate mutation. Page exit and disconnect
  stop UI retries; reconnect cannot resubmit an admitted binding. Verify the
  actual Track state because an admitted mutation may already have applied.
- [ ] Audit follow-up: delay load A progress/completion/failure, leave Browse,
  reopen it and start load B. Deliver A's messages, including duplicates and
  legacy uncorrelated statuses. B's progress, selected sample, overlay and Track
  binding must remain unchanged until B's matching reply. Repeat with resident
  reuse, busy refusal and TX pressure; capture the distinct request/Pool IDs.
- [ ] Audit follow-up: in a diagnostic image omit router injection. Startup
  reports the missing dependency and fails before installing/starting UART.
  Restore normal injection and verify ordinary two-board message delivery.
- [ ] Audit follow-up: read UART diagnostics while injecting RX overflow and
  TX queue rejection on both cores. Verify increments are retained through
  concurrent updates; individual counter reads need not form one snapshot.
- [ ] Encoder accumulation: drive known quadrature edges in both directions across
  both 16-bit hardware limits, reverse near each limit and repeat during flash
  writes/cache-disabled intervals. Consumed movement must equal injected movement
  without jumps or missing detents. Record ISR/task timing and image identity.
- [ ] Directory recovery: drop the first page and a middle page, reject sends,
  and delay old replies across retries/navigation/card removal. At most three
  attempts per page, 1.5 seconds apart; exhaustion stops loading, clears selection
  and offers Retry. Retry must recover without mixed paths or duplicate entries.
- [ ] Busy-overlay failure: refuse sample/Instrument loads and suppress replies
  until timeout. No spinner/bar remains in a terminal state; the reason stays
  visible despite late progress/completion, and Dismiss works on touch. Verify
  a fresh operation completes normally and check the actual backend result.
- [ ] Play labels: compare the octave tile and header to triggered MIDI notes at
  low/high roots and after transposing/switching Pads/Keys. Middle C is C4;
  Pads spans 16 notes and Keys spans 25, with no note-number changes.
- [ ] Root context and dependency wiring: boot, load/unload samples and change
  Track Instruments while moving between root groups. Pool totals and names
  follow backend snapshots; offline/missing data is explicit. Exercise encoder,
  Panel diagnostics, calibration and debug console after the component split.
- [ ] Repeat under sequencer/streaming load; capture UI stack/memory margins,
  link counters and audio underruns. Preserve the independent capacity/soak gate.

**Blocker:** physical/fault-injection session not run for this remediation.

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


## HV-035 — USB MIDI host and port mode

**Status:** Partial. Startup/readback verified and user reports the adapter
working. Source build and eleven ASan/UBSan tests pass; full mode-change,
recovery, persistence and timing acceptance remain open.
**Gate:** [Phase 2.P](roadmap.md#2p--panel-controls-and-midi-io-physical-integration),
[port role contract](features/panel-controls.md#usb-midi-port-roles-2026-09-23).
**Setup:** Record date, both image hashes/versions, board revision, adapter
make/model and USB VID/PID, cable wiring and 5 V VBUS source. Use a directly
connected class-compliant MIDI 1.0 adapter, MIDI source, MIDI capture/DAW,
a sustained gated Instrument, headphones and timing capture. Verify the
connector's power path and supply budget; no software-switched VBUS is added.

- [ ] **035a — Settings / power cycle:** With Device active, change the mode
  using both touch softkeys and encoder. Exit without saving and confirm no
  change. Save Host, wait for saved/restart feedback, and navigate away/back.
  Active must remain Device until restart. Restart with a suitable adapter;
  saved/active must both show Host. Repeat back to Device before connecting a
  computer. Confirm the independent Serial/JTAG console remains accessible.
  Verify scrolling, text fit, idle repaint behavior and UI stack headroom.
- [ ] **035b — Host input / output:** Connect at boot and hot-plug. Confirm
  Connected, channel-aware notes/velocity, note-on-zero releases, CC1, CC121,
  pressure, Program Change and external clock/SPP through existing routes.
  Exercise an input-only device. If OUT exists, capture Clock and
  Start/Continue/Stop/SPP; verify no output-ready claim for input-only devices.
  Verify first-cable isolation on a multi-cable adapter and explicit refusal
  of unsupported devices. Record adapter descriptors and serial diagnostics.
- [ ] **035c — Recovery / ownership:** Unplug with held and repeated notes,
  during RX and output clock traffic. Require no stuck notes, no crash and no
  note release redirected to a different Track. Repeat at least 100 cycles,
  including reconnect during cleanup. Compare free/minimum heap; require no
  progressive leak. Inject transfer stall/error and verify fault feedback,
  bounded cleanup and explicit reconnect recovery. Exercise link congestion
  while releasing; pending releases must recover through the existing service.
- [ ] **035d — Timing / regression:** Repeat HV-014 device enumeration and
  clock checks after switching back. In both modes measure MIDI-in-to-sound
  latency and clock jitter, then run the Phase 2 ten-minute DAW sync workload.
  Include UI navigation, mode-setting saves and current full audio workload;
  record callback DWT peaks, underruns and MIDI/drop counters. These selected
  checks do not replace HV-005/HV-019 one-hour capacity/soak acceptance.
- [ ] **035e — Persistence failure:** Exercise unavailable/full NVS and a
  failed commit in a recoverable test setup. Require visible save failure,
  retained confirmed selection and working retry; calibration must survive.
  Invalid stored mode boots Device with error feedback. Confirm no automatic
  NVS erase or restart while a save is pending.

**Remaining:** Adapter identity/power details and complete acceptance evidence
for the steps above remain outstanding.
Keep the 85.3713% / 86.5040% capacity findings and existing phase gates open.


**2026-09-23 — ESP32 flash and startup only:** Flashed the frontend from
commit `6a69930` through native USB Serial/JTAG; esptool verified all programmed
regions and reset successfully. ESP32 ELF SHA-256:
`76fba82bb40e12cad7d38dfc04a0003a28059552e40f2f3bbb69737cd1ddc129`;
build BIN SHA-256:
`f64143ce60819e9bea79c8021550e303034379417a43e6b4d3b8d19a0978e9e9`.
Daisy was not flashed or tested in this check. Boot reached the application UI,
and acknowledged console navigation reached Settings → MIDI with Device/Host
and Save mode softkeys. Readback: `usbmode=0 usbsaved=0 usbconnection=2
usbsaving=0 usbsaveerror=0` (Device active/saved, waiting, no save error).
Evidence: `logs/usb-host-flash-20260923.log` (local, gitignored). Left the MIDI
page open; no mode save/restart or adapter I/O was exercised. This does not
close 035a–035e, physical rendering, musical timing or any audio/capacity gate.


**2026-09-23 — User confirmation:** After the flash above, the user reported
“works!” for the connected USB MIDI adapter. Record this as a successful
user-reported basic functional check on that flashed ESP32 image, not a measured
latency, clock, reconnect or soak result. Adapter model/VID/PID, exact actions
and MIDI messages exercised were not supplied; the earlier ELF/BIN identities
identify the installed frontend. Full 035a–035e acceptance remains open.
