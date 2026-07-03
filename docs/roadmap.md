# WaveX Implementation Roadmap

**Status**: Canonical implementation-order document. Read `architecture.md` first.
**Last updated**: 2026-07-02

Phases are ordered by dependency, not calendar. Within a phase, items are listed in recommended implementation order. Every phase ends with the test gate that must be green before moving on.

---

## Phase 0 — Foundation Hardening (do first; everything else builds on it)

### 0.1 Toolchain & library upgrades

| Component | Pinned today | Latest | Recommendation |
|---|---|---|---|
| libDaisy | v8.0.0 (submodule) | **v8.1.0** | **Upgrade now.** v8.1.0 contains a reworked `WavPlayer`, new WAV-file utilities, a fix for a compiler-optimization-induced **infinite loop in SDMMC operations**, and SD-card error handling via HAL callbacks — all directly on our critical path (SDMMC streaming). |
| CMSIS-DSP | 1.14.4 (vendored inside libDaisy `Drivers/CMSIS-DSP`) | **1.17.0** | Upgrade opportunistically, not urgently. 1.15–1.17 fixes are mostly Helium/Neon (irrelevant on Cortex-M7: we use the DSP extension + FPU paths) plus a few generic out-of-bounds fixes in biquad/FIR-interpolation — relevant once offline DSP uses those kernels. Since it ships inside libDaisy, prefer bumping the submodule pointer within our libDaisy fork rather than vendoring separately. |
| ESP-IDF | 5.5.1 | 5.5.x (bugfix) / **6.0 (Mar 2026)** | **Stay on 5.5.x for now; track patch releases.** Plan the 6.0 migration as a dedicated task, not a drive-by. |
| LVGL / esp_lvgl_port | ~~9.3.0 / 2.x~~ **9.5.0 / 2.8.0** (managed components) | 9.5.x | **Done, compile/build-verified only — needs a real-panel check.** `idf_component.yml` now pins `lvgl/lvgl: '>=9.4,<10'`; the component manager resolved 9.5.0 + `esp_lvgl_port` 2.8.0. `CONFIG_LVGL_PORT_ENABLE_PPA=y` set in both `sdkconfig` and `sdkconfig.defaults`. **Correction to the risk note below: our UI *does* use rotation** (`display_manager.cpp` calls `lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90)`), and `LVGL_PORT_ENABLE_PPA`'s own Kconfig help text is literally "Enable PPA for screen rotation" — so this is likely the intended optimization for exactly our case, but it also means we're not exempt from the "known PPA interaction bugs in rotation modes on P4" caution below as the original note assumed. **Flash the HX8394/MIPI-DSI panel and watch for tearing/corruption during rotation before trusting this in the field** — full ESP32 build is green in the devcontainer, but that only proves it compiles and links, not that the panel renders correctly. If it misbehaves, flip `CONFIG_LVGL_PORT_ENABLE_PPA` back to unset in both sdkconfig files. |
| DaisySP | V1.0.0+25 commits | rolling | Fine; update with libDaisy. |

**Upgrade risk callouts**

- **libDaisy v8.0.0 → v8.1.0**: `WavPlayer` was reworked — our `audio_engine.cpp` does not use `WavPlayer` (custom streaming), so risk is low, but the SDMMC and `fatfs` glue changed; re-run the SD soak test (mount, 1000× sequential reads, hot-unmount) after bumping. Our `sd_sdio.cpp` wraps `SdmmcHandler`; check for signature changes in error-callback registration.
- **CMSIS-DSP 1.14.4 → 1.17.0**: build-system layout changed upstream (standalone pack vs CMSIS_5 bundle); libDaisy vendors it as a submodule, so the clean path is aligning with whatever libDaisy master pins. Do not hand-copy sources.
- ~~**LVGL 9.3 → 9.4/9.5**: low risk (minor-version API stability within 9.x; esp_lvgl_port supports LVGL 8 and 9). Two checks: (1) enable `CONFIG_LVGL_PORT_ENABLE_PPA` and verify no regressions with our HX8394/MIPI-DSI path — there are known PPA interaction bugs in some rotation/tearing modes on P4, and we use neither rotation nor triple-partial mode, so exposure is low; (2) rebuild the waveform-preview and meter pages and profile before/after (FPS + UI-task CPU) to confirm the win is real on our panel.~~ **Version bump done** (9.3.0 → 9.5.0), build-verified, no API breakage hit. **The "we use neither rotation..." premise here was wrong** — `display_manager.cpp` sets `LV_DISPLAY_ROTATION_90` — so check (1) is *more* important than originally assessed, not less; do it before flashing to the field. Check (2) (FPS/CPU before-after profiling on the waveform-preview and meter pages) is still fully outstanding — needs the DWT/`docs/performance_monitoring.md` workflow on real hardware, nothing here substitutes for it.
- **ESP-IDF 5.5 → 6.0**: three known tripwires for us:
  1. **ESP32-P4 default chip revision becomes v3.0** — binaries won't boot on rev < 3.0 silicon unless `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. Check our module's silicon rev before migrating.
  2. Deprecated legacy drivers are removed (we already use new-style `spi_slave`, `pcnt` — audit `i2c` legacy usage in third-party components: `esp_tca8418` is fetched from git and may lag).
  3. Managed components (`esp_lvgl_port`, waveshare BSP, hx8394/gt911 drivers) must all declare 6.0 compatibility; the waveshare BSP is the likely laggard. Budget a spike branch to test the build before committing.

### 0.2 Repo & contract cleanup (small, high leverage)

1. **Delete legacy SD backend**: `sd_spi.cpp`/`diskio_sd_spi.cpp` (kept in git history). `WAVEX_DAISY_SD_CARD_BACKEND` defaults to `1` (SDIO) and nothing sets it to `0`, so the SPI SD path was dead. Done — `make daisy` + `make test` green with the files removed.
   - **`esp_uart_link`/`daisy_uart_link` are NOT dead and were not touched.** They're a live transport running in parallel with SPI: SPI carries browse/wave data only, while UART carries heartbeat, meter-push, status, and ACK/NACK messages on both MCUs (see `main.cpp`'s "SPI reserved for file browser and wave data only" comment, and `inter_mcu.cpp` on the ESP32 side). Consolidating onto SPI-only is real protocol/transport work — extending `protocol.h` to carry the UART-only message types, rewiring every `UartLinkSend`/`uart_link_send` call site, then hardware bring-up to verify — not a drive-by deletion. Track as a separate task if/when SPI-only consolidation is prioritized.
2. ~~**Fix `partitions.csv`** (uses 2 MB of 16 MB flash; vestigial "samples" partition). Add OTA slots while at it.~~ **Done.** New table maps the full 16 MB: factory fallback + two 4 MB OTA app slots with `otadata`, `userdata` grown to ~3.9 MB, vestigial `samples` partition dropped (nothing referenced it by name; samples live on the Daisy's SD). App binary (815 KB) now has 81% headroom in its slot. Devcontainer build green; **flash + boot on hardware still to be verified** (app offset moved 0x10000 → 0x20000; `flash-esp32.sh` uses `idf.py flash` so no script changes needed, but NVS content survives only because `nvs` kept its old offset/size).
3. ~~**Wire-struct hygiene**: add named constructors / encode-decode helpers for every message in `protocol.h`; forbid aggregate-initializing packed wire structs in app code and tests. Round-trip tests for every message type (some exist; make it exhaustive).~~ **Done.** Every message struct has a default + named-argument constructor and no others, so aggregate/designated-init is now a compile error; all call sites migrated. Round-trip tests are exhaustive (added the 4 previously-untested types). This work found and fixed a real `ParseWaveXPacket` buffer overflow (see `CHANGELOG.md`) — writing the tests, not just the constructors, is what caught it.
4. ~~**One event-dispatch owner on ESP32**: collapse the overlap between `inter_mcu`, `PacketRouter`, `ListenersManager`, `StatisticsManager` → `PacketRouter` owns fan-out; `inter_mcu` becomes a thin facade. (Assessment recommendation #2, still open.)~~ **Done.** `ListenersManager` (`comm/listeners.h/.cpp`) was wholesale dead code (zero instantiations anywhere) — deleted. `inter_mcu_set_meter_listener`/`_set_browse_resp_listener` were dead redundant entry points into `StatisticsManager` (the live registration path is `CommInterfaceImpl` → `StatisticsManager` directly) — deleted, along with the always-losing `s_sample_status_listener` fallback branch (`s_statistics` always wins in production, so the branch was unreachable dead weight). Two dead duplicate SPI dispatch functions (`handle_control_message_from_daisy`, `handle_control_message_from_daisy_new_format`, zero callers) and the byte-unpacking `inter_mcu_process_daisy_control_message` they alone called (duplicating what `PacketRouter::handle_meter_push`/`handle_heartbeat` already do with typed structs) were deleted. Fixed a real, previously-disabled bug along the way: `application_context.cpp`'s UART `PacketRouter` injection was commented out with "function not linking properly" — the actual cause was a namespace-scoping bug (the `extern` declaration was lexically inside `namespace WaveX`, declaring the wrong symbol); fixed by including the real header and qualifying the call with `::`. Production UART traffic now routes through `ApplicationContext`'s single owned `PacketRouter` instead of a throwaway `dummy_router` fallback. `inter_mcu_set_wave_chunk_listener`/`_set_sample_status_listener` were left alone — they're genuinely live, used by production UI code (`ui_sample_record_page.cpp`, `ui_sample_edit_page.cpp`, `ui_sample_browser.cpp`), and `ICommInterface` has no wave-chunk equivalent to consolidate onto; migrating those is separate, riskier work for when UI changes can be verified on hardware.
5. **Split `daisy_spi_link.cpp`** along the boundaries in `archive/daisy_spi_link_splitup_plan.md` (transport / packet / message-processing / bridges) — do it opportunistically as Phase 1 touches those files, not as a big-bang refactor.
6. ~~**Fix test-build brittleness**: fresh build dirs by default; vendor or cache GoogleTest so `make test` works offline.~~ **Done.** GoogleTest is now a single vendored submodule at `firmware/shared/tests/_deps/googletest-src` (pinned `release-1.12.1`), referenced by all three test `CMakeLists.txt` via `FetchContent_Declare(... SOURCE_DIR ...)` instead of `GIT_REPOSITORY`. Also fixed a duplicated/malformed nested submodule entry (`.../tests/_deps/firmware/daisy/tests/_deps/googletest-src`) that had accumulated from a prior `git submodule add` run from the wrong directory. `make test-daisy`/`test-esp32`/`test-shared` now `rm -rf` their build dirs before reconfiguring.

**Gate**: `make all` + `make test` clean from scratch; SD soak test passes on v8.1.0.

---

## Phase 1 — Solid Playback Core (the instrument must play reliably before it grooves)

> ### ⚠ Next steps first: DMA/timing review findings (2026-07-03)
>
> A dedicated review of the UART link and audio path (**[`dma-timing-review-2026-07-03.md`](dma-timing-review-2026-07-03.md)**) found 12 issues, three high-severity. **Work through these before continuing with items 5/6/8 below** — items 5 (CV at the control tick) and 8 (MIDI over UART), and all of Phase 2 (sequencer tempo), build directly on the affected code.
>
> 1. ~~**Switch the engine back to 48 kHz** (decision recorded 2026-07-03).~~ **Done** (with the Finding-6 resample-fallback fix landing first as a prerequisite — the resample path is now the *normal* path for 44.1 kHz content). `main.cpp` → `SAI_48KHZ`; `timebase.hpp` → 48000 with a `static_assert` enforcing the integer-ms invariant; `WAVEX_AUDIO_SAMPLE_RATE` → 48000; `VoiceTriggerParams::sample_rate_hz` added for RAM-resident playback-rate compensation (`Voice::increment` scaled by native/engine — host-tested, incl. composition with note pitch). Resample-on-load remains a later Phase-4-friendly option. **Needs a hardware listen test**: audition a 44.1 kHz and a 48 kHz WAV, confirm correct pitch on both.
> 2. **Fix the oversize-frame transmit wedge** (Finding 2): frames ≥ ~1990 bytes deterministically exceed the 10 ms `BlockingTransmit` timeout at 2 Mbaud and can never be sent — each attempt sprays a truncated frame and blocks the main loop, starving audio for ~1 s until the frame is dropped. Wave-chunk single frames are only 0.9 ms under this cliff today.
> 3. **Fix the UART-over-audio interrupt priority inversion** (Finding 3): libDaisy leaves UART4_IRQn and its RX DMA stream at NVIC priority 0, above audio's 5 — two `HAL_NVIC_SetPriority` calls after `UartLinkInit`.
> 4. Then the medium findings (4–8: CRC-under-IRQ-lock, dropped preview chunks, wrong-pitch resample fallback, per-iteration CRC-recovery window, ~8.3 KB of DMA RAM wasted on a non-DMA queue) and low findings (9–12, ESP32-side), per the review doc.

Order of implementation:

1. ~~**Output/CV backend seams first** (`architecture.md` §5.3): introduce the `WAVEX_VOICE_OUTPUT_BACKEND` / `WAVEX_CV_BACKEND` / `WAVEX_ANALOG_CV_GROUPS` flags in `hardware_config.h`, the output-sink split (`StereoMixSink` now, `TdmVoiceSink` stub), and the CV group router over `cv_bus`. Cheap while the engine is small; retrofitting after the voice manager exists is expensive. CI compiles both flag sets.~~ **Done** — see `architecture.md` §5.3 "Implementation status". `cv_bus.hpp` (the old orphaned single-group prototype, `Flush()` was hardcoded to slot 0 regardless of caller) was superseded by `firmware/daisy/src/cv/{cv_cal,cv_group_router,mcp4728_backend,mcp48_backend}.hpp`. Both flag-set combinations build in CI (`make daisy` + new `make daisy-stageb`, `.github/workflows/ci.yml`). Not yet wired into the audio callback — nothing to feed the sink/router until item 2 (voice manager) exists; the paraphonic envelope law is item 5's job, not this seam's.
2. **Voice manager**: 8 voices, allocation/stealing, per-voice gain/pan/pitch; RAM-resident samples trigger with zero I/O. Streamed voices limited to (initially) 2 concurrent streams with prebuffer admission control. Voices render to per-voice buffers consumed by the active sink.
   - ~~RAM-resident half~~ **Done.** `firmware/daisy/src/audio/voice_manager.hpp` — `VoiceManager`, HAL-free (host-testable, no daisy_seed.h dependency), 8-voice array, oldest-triggered stealing, per-voice gain (from velocity)/pan/pitch (`increment`, hardcoded 1.0 for now — pitch-from-note mapping is item 4's job), renders into the `output_sink.hpp` buffer shape from item 1. 12 new host tests. Constructed in `audio_engine.cpp` (proven to compile for the real ARM target) but **not wired into `Callback()`** — `OnNoteOn`/`OnNoteOff` still only drive the test oscillator, because there's no note-to-sample mapping policy yet (which MIDI note plays which loaded sample); that's item 8's job ("MIDI note path ... → voice manager").
   - **Streamed-voice concurrency (2 concurrent streams + prebuffer admission control) is a separate, not-yet-started follow-up.** The existing WAV-streaming path (`s_wav`, ring buffer, prebuffer, SD buffer slots — all in `audio_engine.cpp`) is entirely singleton/global today; generalizing it to N concurrent admission-controlled streams is a distinct refactor of that subsystem, not an extension of `VoiceManager` (which only handles RAM-resident playback). Scoped out of this pass to avoid a second large, hardware-unverifiable change in the same commit.
3. ~~**Sample RAM integration for recording**: replace `Sampler`'s `std::vector` heap growth with preallocated extents from the SDRAM allocator (`memory.h`); recording must be real-time-safe (no `reserve()` in the audio path).~~ **Done.** `Sampler::Init()` now takes a `SampleMemMgr&` and preallocates a fixed-capacity extent (30s @ sample rate by default in `audio_engine.cpp`) once at setup time; `FeedInputBlock()` writes within that fixed capacity and stops (doesn't grow) once full. Found in the process: `FeedInputBlock()` was never actually called from `Callback()` in production — recording is currently wired up (`OnSampleCtrl` → `StartRec`/`StopRec`) but no live audio input is fed into the buffer, so recording produces silence today regardless of this fix. Making the storage real-time-safe doesn't make recording functional; wiring `Callback()`'s discarded `in` buffer into `FeedInputBlock()` is separate, not-yet-scoped work.
4. ~~**Per-voice digital processing (playback-time, non-destructive)**: interpolated pitch (exists: `arm_linear_interp_q15`), start/end/loop, one-pole/SVF digital filter as stand-in until the analog board exists, ADSR per voice.~~ **Done.** `VoiceManager`/`Voice` (from item 2) extended: pitch is a 12-TET ratio from `note` relative to a per-trigger `root_note`; start/end/loop points (`Voice::start_frame/end_frame/loop/loop_start/loop_end`); a one-pole lowpass (`audio/one_pole_filter.hpp`, picked over SVF for simplicity/portability — no per-voice resonance state needed for a stand-in); a linear ADSR (`audio/envelope.hpp`, not DaisySP's `Adsr` — that isn't host-portable). `Trigger()`'s signature grew to a `VoiceTriggerParams` struct (named fields, not an 11-argument positional call) with sensible defaults. Voice-stealing now prefers a voice already in its release tail over an older sustaining one. **Did not adopt `arm_linear_interp_q15`** — the roadmap cites it as "(exists)" for later use, not a mandate for this pass; swapping the portable float interpolation for a q15 CMSIS kernel is a profiling-driven ARM-only optimization (needs the DWT cycle counter on real hardware per `docs/performance_monitoring.md`, not available here) that would cost this class's host-testability. 10 new host tests (22 total for `VoiceManagerTest`).
5. **Stage A paraphonic analog path** (`features/analog-voice-board.md` §0): stereo codec out → shared VCF/VCA, MCP4728 CV at the 1 kHz tick, paraphonic envelope law (retrigger on note-on, release on last voice), calibration workflow + UI page. This validates the entire CV/calibration stack on breadboard hardware before the voice-board PCB exists.
6. **Raise SPI link clock**: replace bring-up `PS_16` prescaler; verify on scope, measure error rate with packet-statistics counters at each step. Target: browse a 500-entry directory in < 500 ms; waveform preview of a 3-min WAV in < 1 s.
7. ~~**Link robustness regression tests**: either MCU rebooting never wedges the other (ATTN stuck-high recovery, sequence-number resync). This was a real bug in the UART era; keep the test.~~ **Done**, scoped to what's real and host-testable. Investigation found: the sequence-number validation that exists (the disabled SPI path's `is_duplicate_packet`, byte-for-byte duplicated between `daisy_spi_link.cpp`/`esp_spi_link.cpp`) would have **actively wedged** on a real reboot — a peer resetting its seq counter to 1 gets permanently classified "out-of-order" against the receiver's still-high expectation, with no recovery path. Fixed by extracting a shared `firmware/shared/spi_protocol/sequence_tracker.hpp` (`SequenceTracker`) that detects the reboot signature (low, fresh-looking seq arriving far below an already-advanced expectation) and resyncs instead of wedging. Also added `firmware/shared/spi_protocol/attn_watchdog.hpp` (`AttnWatchdog`) — there was **no timeout at all** on the ESP32 side for "asserted ATTN, transaction never completed," so a wedged Daisy left ATTN stuck high forever; now force-deasserted after 500ms (mirroring the UART fix's threshold). Both classes are HAL-free/host-tested (12 new tests) since real GPIO/SPI timing can't be host-tested. Found and fixed, in passing, the *same* namespace-scoping bug class as the UART `PacketRouter` injection fix (Phase 0.2): `daisy_spi_link.cpp`'s `extern s_hw` was declared at global scope instead of `WaveX::Comm` — this SPI code doesn't compile-check at all in the default build (`WAVEX_SPI_LINK_ENABLED=0` wraps the entire file), so it had silently bit-rotted; verified by temporarily flipping the flag to 1, confirming a clean build+link on both MCUs, then reverting. The UART-era historical bug itself (100ms blocking transmit freezing the main loop) is already fixed in shipping code (`daisy_uart_link.cpp`, 10ms timeout + 500ms/1000ms stuck-TX force-clear) but reproducing the exact hardware timing scenario needs real hardware; not touched further here to avoid refactoring live, working code for testability alone.
8. **MIDI note path**: DIN + USB MIDI in → `MSG_NOTE_ON/OFF` → voice manager, velocity mapped. Latency budget: < 5 ms in-to-sound.

**Gate**: 8-voice drum kit playable from MIDI with zero underruns for 1 hour; paraphonic analog path (Stage A) calibrated and audible; both output/CV flag configurations compile in CI; host tests cover voice allocation and the sample-load → status → UI flow.

---

## Phase 2 — Groovebox Core: Sequencer + Pads (see `features/sequencer.md`)

1. Sequencer engine **on the Daisy** (sample-accurate, driven by the 1 kHz control tick with sample-offset scheduling inside the block).
2. Pattern model: 16 steps × pages, per-step note/velocity/probability/micro-timing; kits map pads → samples + voice params.
3. Transport & sync: internal clock, MIDI clock out, then MIDI clock in (slave) — tempo drift test against a reference clock.
4. New protocol messages: pattern edit ops (UI → engine), playhead/step feedback (engine → UI, coalesced), kit management. Extend `protocol.h` with round-trip tests **before** UI work.
5. UI: pad grid page (TCA8418 matrix + touch), step editor page, kit editor. LED feedback via TLC5947 (bring up SPI2 driver here — first real consumer).
6. Project persistence on SD (kits/patterns/songs); atomic save (temp + rename). Format doc before code.

**Gate**: program and perform a 4-track drum pattern with swing from the front panel; MIDI-clock-synced to a DAW without audible drift over 10 minutes.

---

## Phase 3 — Analog Voice Board (see `features/analog-voice-board.md`)

This phase is the **Stage A → Stage B transition** (`features/analog-voice-board.md` §0): the paraphonic prototype from Phase 1 already validated CV calibration, envelope→CV timing, and the analog levels, so this phase is hardware bring-up plus a flag flip — not new engine architecture. Blocked on hardware decision §3.3 of `architecture.md` (Stage B CV DAC part).

1. Decide Stage B CV DAC (recommendation: SPI MCP48CMB28 chain) and voice count; freeze PCB spec.
2. PCM1690 bring-up: SAI2 TDM-8 master TX, 8 test tones to verify slot order; I2C register init (reset sequencing, 24-bit TDM format, unmute).
3. `TdmVoiceSink` / `AudioOutputMode::VoiceSAI2` path: per-voice → TDM slot interleave (`float_to_int24`, exists in design), SAI1 stays stereo input + master mix.
4. `Mcp48Backend` behind the CV group router; flip flags to `TDM8` / `MCP48` / 8 groups; DMA/IT flush from main loop — never blocking I2C/SPI in the callback.
5. Re-run calibration per voice (procedure and UI page reused from Stage A); stored calibration table on SD.
6. Keep Stage A buildable in CI as the fallback/bring-up configuration.

**Gate**: 8 analog voices with per-voice cutoff/res/VCA under sequencer control; calibration survives power cycle; scope-verified CV update ≤ 1 ms after control tick.

---

## Phase 4 — Offline Sample Editing & Mangling (see `features/offline-sample-editing.md`)

1. Render-job scheduler on the Daisy main loop (chunked SD→SD processing with progress messages; cancellation).
2. Editing primitives: trim/crop, gain/normalize, fades, reverse, mono↔stereo, resample.
3. Waveform editor UI: zoomable preview (decimated tiers cached on ESP32 PSRAM), region selection with encoder fine-adjust, non-destructive markers (start/end/loop/slices) stored in a sidecar, destructive ops via render jobs.
4. Slicing: transient detection (offline), slice-to-pads workflow.
5. Mangling effects (offline renders): bit-crush, drive/saturate, time-stretch, pitch-shift, granular freeze. CMSIS-DSP kernels where applicable — this is where the 1.17.0 upgrade pays off.

**Gate**: record → trim → normalize → slice → assign to pads → sequence, entirely on-device, with audio playback uninterrupted during renders.

---

## Phase 5 — Performance & Polish

- Song mode / pattern chaining; performance macros (encoder-assignable).
- Digital send FX (delay, reverb) in the stereo master section.
- Preset/kit browser richness (tagging, favorites), USB sample import (MSC or MTP — decide), settings persistence.
- ESP-IDF 6.0 migration (after the ecosystem components support it — see 0.1).
- CPU/memory headroom pass with DWT profiling; lock the final block-size and clock decisions.

---

## Cross-Cutting Rules (apply to every phase)

- Every protocol change: update `protocol.h` + round-trip test + `features/inter-mcu-protocol.md` in the same commit.
- Every DMA buffer: alignment + placement per `architecture.md` §7 — reviewer checklist item.
- Every phase gate includes: 1-hour zero-underrun soak, both-MCU-reboot recovery test, `make test` green.
- Docs: new subsystems get a `docs/features/*.md` design doc **before** implementation; superseded docs move to `docs/archive/` (never silently deleted).
