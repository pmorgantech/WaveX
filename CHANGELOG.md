# Changelog

All notable changes to WaveX are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Frontend (ESP32-P4) and backend (Daisy Seed) firmware share a single version
number, defined in the root [`VERSION`](VERSION) file. See `AGENTS.md` for the
versioning and release process.

## [Unreleased]

### Fixed (Daisy link) — DMA/timing review Finding 2

- **Frames ≥ ~1990 bytes are no longer deterministically un-sendable.** The
  UART blocking-transmit timeout was a fixed 10 ms, but a max frame (2058
  bytes) at 2 Mbaud needs 10.29 ms of wire time — so near-max frames always
  timed out mid-frame, sprayed a truncated frame at the peer (CRC/sync
  storm), and retried the whole frame every main-loop pass (~10 ms blocked
  each) until the 1-second stuck-TX force-clear dropped the message,
  starving the audio pump throughout. The timeout is now derived from the
  frame's own wire time plus margin (`UartTxTimeoutMs()` in
  `uart_protocol.h`, host-tested — including a regression test documenting
  that the max frame's wire time really did exceed the old fixed timeout).
  Documented tradeoff: a max-size frame now legitimately blocks ~13 ms,
  slightly over §7.1.4's ~10 ms guideline, until TX moves to DMA; typical
  traffic stays well under 10 ms.

### Fixed (Daisy link) — DMA/timing review Finding 3

- **UART interrupts no longer preempt audio.** libDaisy hardcodes UART4_IRQn
  (`HAL_UART_MspInit`) and DMA1_Stream5 / UART4 RX DMA (`dsy_dma_init`) to
  NVIC priority (0,0) — the maximum, above the audio SAI DMA at 5 — letting
  the UART RX callback's multi-KB memmoves preempt the audio callback,
  inverting architecture.md §7.1.5's "audio highest" rule. `main.cpp` now
  re-sets both to priority 7 (below audio 5/6, above SPI 10) immediately
  after `UartLinkStart()`. Compile-verified; the jitter improvement needs
  DWT measurement on hardware.

### Changed (CI)

- GitHub Actions now runs inside `espressif/idf:release-v5.5` — the same
  base image as `.devcontainer/Dockerfile` — instead of git-cloning ESP-IDF
  v5.2 from scratch every run. This fixes the toolchain-version mismatch
  with dev (5.5 vs 5.2), removes ~10 minutes of per-run IDF install, and
  makes the Daisy toolchain identical too (the image's
  `gcc-arm-none-eabi` is 13.2.1, matching dev). The stale build-directory
  caches (which risked restoring stale absolute paths) were dropped along
  with the separate IDF/ARM-GCC install steps. The full new CI recipe —
  fresh recursive clone through both firmware builds and all host tests —
  was dry-run locally in the exact container image before landing.
  Cosmetic: `make esp32`'s banner no longer claims "ESP32-S3 / ESP-IDF
  v5.2" (it's P4 / release-v5.5).

### Changed (submodules)

- **libDaisy submodule re-pinned to the public upstream v8.1.0 tag**
  (`9498417a`), and its nested `Drivers/STM32H7xx_HAL_Driver` to the
  upstream commit v8.1.0 ships with (`404a70d`). The local-only patch
  commits (44.1 kHz SAI support, ADC HAL sources disabled, `stdint.h`
  include fix) are now fully obsolete: the 48 kHz switch removed the only
  consumer of the SAI patch, and a clean-from-scratch build against pure
  upstream confirms the ADC/stdint build fixes are no longer needed (the
  CMSIS-include fix in `firmware/daisy/CMakeLists.txt` covers it). This
  unblocks pushing the repo: fresh clones and CI (`submodules: recursive`
  checkout) can now fetch every pinned submodule commit from its public
  upstream. The old patches remain available locally on the
  `wavex-v8.1.0-with-local-patches` branch in the submodule clone if ever
  needed for reference.

### Changed (Daisy audio) — needs hardware listen test

- **Engine sample rate: 44.1 kHz → 48 kHz** (decision 2026-07-03,
  `docs/dma-timing-review-2026-07-03.md` Finding 1). Restores the
  1-block = 1-ms control-tick invariant (48-sample blocks at 48 kHz), which
  44.1 kHz had silently broken — the "1 kHz" tick was running at 918.75 Hz,
  and a Phase 2 sequencer built on it would have run ~110 BPM at a setting
  of 120. `timebase.hpp` now `static_assert`s the integer-ms invariant so
  this can't silently regress again. 44.1 kHz WAV content is rate-converted
  at playback: streaming/audition through `PumpWavIO`'s existing resampler
  (made trustworthy by the Finding-6 fix in the previous commit, since
  resampling is now the *normal* path for 44.1k files), and RAM-resident
  samples via new playback-rate compensation —
  `VoiceTriggerParams::sample_rate_hz` scales `Voice::increment` by
  native/engine rate (0.91875 for 44.1k on 48k), composing multiplicatively
  with note pitch; 3 new host tests. Resample-on-load stays a later option
  for Phase 4 uniformity. **Verify on hardware**: audition one 44.1 kHz and
  one 48 kHz WAV and confirm correct pitch on both.

### Added

- **DMA/timing code review** (`docs/dma-timing-review-2026-07-03.md`): 12
  findings across the UART link and audio path, three high-severity — the
  control tick actually runs at 918.75 Hz not 1 kHz (engine is at 44.1 kHz
  with 48-sample blocks, violating architecture.md §5.1's 1-block=1-ms
  invariant; **decision recorded: return to 48 kHz**, with 44.1 kHz WAVs
  handled by the existing streaming resampler plus playback-rate
  compensation in `VoiceManager`); Daisy→ESP32 frames ≥ ~1990 bytes
  deterministically exceed the 10 ms `BlockingTransmit` timeout at 2 Mbaud
  and can never transmit (retry storm starves audio ~1 s per attempt-cycle);
  and libDaisy leaves UART4 + its RX DMA stream at NVIC priority 0, above
  audio's 5 (priority inversion vs §7.1.5). Fix sequencing added to the
  front of `roadmap.md`'s Phase 1 next steps; no code changed in this
  commit.

- **Link robustness regression tests** (roadmap Phase 1 item 7): investigation
  found the disabled SPI path's sequence-number validation
  (`is_duplicate_packet`, byte-for-byte duplicated between
  `daisy_spi_link.cpp`/`esp_spi_link.cpp`) would have permanently wedged on
  a real peer reboot — a sender resetting its sequence counter to 1 gets
  classified "out-of-order" against the receiver's still-high expectation
  forever, with no recovery path. Extracted into a single shared
  `firmware/shared/spi_protocol/sequence_tracker.hpp` (`SequenceTracker`)
  that detects that specific reboot signature (a low, fresh-looking
  sequence number arriving far below an already-advanced expectation) and
  resyncs instead of wedging, replacing both duplicated copies. Added
  `firmware/shared/spi_protocol/attn_watchdog.hpp` (`AttnWatchdog`): the
  ESP32-side ATTN-assertion code had **no timeout at all** for "asserted
  ATTN, transaction never completed" — a wedged Daisy left ATTN stuck high
  forever with no recovery; now force-deasserted after 500ms, mirroring the
  UART fix's threshold. Both are HAL-free/host-tested (12 new tests) since
  real GPIO/SPI electrical timing isn't testable without hardware — see the
  roadmap entry for exactly what is and isn't covered.

- Synced versioning: a single root `VERSION` file now drives both the ESP32
  (`PROJECT_VER`) and Daisy (`project(... VERSION ...)`) firmware builds.
- `AGENTS.md` / `CLAUDE.md` project instructions.
- **Output/CV backend seam** (roadmap Phase 1 item 1, architecture.md §5.3):
  `WAVEX_VOICE_OUTPUT_BACKEND`/`WAVEX_CV_BACKEND`/`WAVEX_ANALOG_CV_GROUPS`
  flags (`hardware_config.h`); `StereoMixSink` (real, sums per-voice buffers
  into the SAI1 stereo stream) and `TdmVoiceSink` (compiling stub for the
  Phase 3 PCM1690/SAI2 bring-up) in
  `firmware/daisy/src/audio/output_sink.hpp`; a template-based
  `CvGroupRouter` (`firmware/daisy/src/cv/cv_group_router.hpp`, zero vtable
  overhead) that folds voice-indexed CV targets onto
  `WAVEX_ANALOG_CV_GROUPS` physical groups, backed by `Mcp4728Backend` (real
  I2C, Stage A) or `Mcp48Backend` (compiling stub, Stage B). New CMake cache
  vars in `firmware/daisy/CMakeLists.txt` plus a `make daisy-stageb` target
  and CI step prove both flag-set combinations compile. 10 new host tests
  for the router's voice-to-group folding and the sinks' mixing logic (both
  are HAL-free and run without any Daisy hardware).
  **Not wired into the audio callback yet** — today's engine has exactly one
  playback source (the WAV ring buffer / `Sampler`), not an array of
  per-voice buffers, so there's nothing for the sink/router to consume until
  the voice manager (Phase 1 item 2) exists. The paraphonic envelope law
  (which voice's envelope drives the shared Stage A VCF/VCA) is Phase 1 item
  5's job, not this seam's.
- **Voice manager, RAM-resident half** (roadmap Phase 1 item 2):
  `firmware/daisy/src/audio/voice_manager.hpp` — `VoiceManager`, 8-voice
  array, allocation with oldest-triggered stealing when all 8 are busy,
  per-voice gain (from MIDI velocity) and linear pan, a pitch hook
  (`Voice::increment`, hardcoded to 1.0 - note-to-pitch mapping is item 4's
  job), zero-I/O triggering (`Trigger()` just stores a pointer into
  already-SDRAM-resident sample data - no allocation, no blocking), and
  `Render()` into the stereo buffer shape `output_sink.hpp` (item 1)
  consumes. HAL-free like the CV router, so it's host-testable without any
  Daisy hardware; 12 new tests cover allocation, stealing, release-by-note,
  gain/pan scaling, self-stop at sample end (no looping yet), and rejecting
  null/too-short samples. Constructed in `audio_engine.cpp` (compiles for
  the real ARM target - a first pass at this forgot to actually `#include`
  it into any compiled translation unit, so the ARM build silently never
  checked it; caught before committing) but **not wired into `Callback()`**
  — `OnNoteOn`/`OnNoteOff` still only drive the test oscillator, since
  there's no note-to-sample mapping policy yet (item 8's job).
  **Streamed-voice concurrency (2 concurrent streams + prebuffer admission
  control), the other half of item 2's roadmap text, is not started** — the
  existing WAV-streaming path is a separate, still-singleton subsystem; see
  `docs/roadmap.md` item 2 for why it's scoped as its own follow-up rather
  than bundled into this change.
- **Per-voice digital processing** (roadmap Phase 1 item 4): extends the
  voice manager above with the pieces it was deliberately missing —
  `Voice::increment` is now a real 12-TET pitch ratio (`note` relative to a
  per-trigger `root_note`, octave up/down verified exactly); start/end/loop
  playback region (`Voice::start_frame/end_frame/loop/loop_start/loop_end`
  — a looping voice now keeps playing indefinitely instead of self-stopping
  at the sample's natural end); a one-pole lowpass filter stand-in for the
  analog VCF (`firmware/daisy/src/audio/one_pole_filter.hpp`, picked over
  SVF for simplicity — no per-voice resonance state needed for "a
  stand-in"); and a linear ADSR envelope
  (`firmware/daisy/src/audio/envelope.hpp` — deliberately not DaisySP's
  `Adsr`, which isn't linked into the host test libraries). `Release()` now
  starts the envelope's release phase instead of hard-stopping the voice —
  a voice stays allocated and rendering through its release tail, matching
  real synth behavior; this is a real, deliberate behavior change from the
  item-2 version. Voice-stealing now prefers a voice already releasing over
  an older sustaining one (the follow-up the item-2 code comment predicted).
  `Trigger()`'s signature changed to a `VoiceTriggerParams` struct (named
  fields) since the old 5-positional-argument form doesn't scale to this
  many parameters. **Did not adopt `arm_linear_interp_q15`** despite the
  roadmap citing it — that's a profiling-driven ARM-only optimization
  (needs the DWT cycle counter on real hardware) that would cost this
  class's host-testability; the roadmap text reads as "(exists) for later
  use," not a mandate for this pass. 10 new host tests (22 total).
  Still not wired into `Callback()`, same reasoning as item 2.

### Fixed (Daisy SPI link) — roadmap Phase 1 item 7

- **`daisy_spi_link.cpp`'s `extern daisy::DaisySeed* s_hw;` was declared at
  file/global scope**, before any `namespace WaveX::Comm` block in the file
  opens — the same namespace-scoping bug class as the UART `PacketRouter`
  injection fix (Phase 0.2, `c3c7967`). The real `s_hw` is
  `WaveX::Comm::s_hw`, defined in `daisy_uart_link.cpp`; the mismatched
  extern declared a different, never-defined `::s_hw`. Harmless while
  `WAVEX_SPI_LINK_ENABLED=0` wraps this entire file out of the build (the
  default), which is exactly why nobody had noticed: this dead code hadn't
  actually compile-checked, let alone linked, in some time. Found and fixed
  while verifying the sequence-resync/ATTN-watchdog changes below actually
  compile — temporarily flipped the flag to 1, confirmed the link error, hit
  this bug, fixed it, confirmed a clean build+link on both MCUs, then
  reverted the flag (SPI stays disabled; re-enabling it for real is roadmap
  Phase 1 item 6, blocked on oscilloscope access to verify the raised clock).

### Fixed (Daisy audio) — roadmap Phase 1 item 3

- **`Sampler`'s recording buffer no longer grows via `std::vector::push_back`
  in the audio path.** `Sampler::Init()` now takes a `SampleMemMgr&` and a
  frame-count capacity, and preallocates one fixed-size extent from it up
  front (`audio_engine.cpp` requests 30 seconds at the current sample rate,
  right after `SampleMemMgr::init()` — reordered so the manager exists
  before `Sampler` allocates from it). `FeedInputBlock()` now writes within
  that fixed capacity and silently stops once full, instead of triggering a
  heap reallocation on every sample past the old 1024-sample
  `reserve()` (AGENTS.md constraint #1: no heap allocation in the audio
  path). **Found in the process**: `FeedInputBlock()` was never actually
  called from `Callback()` in production (only from tests) — recording is
  wired up via `OnSampleCtrl`'s `StartRec`/`StopRec`, but `Callback()`
  discards its input buffer (`(void)in;`) and never feeds it to the
  sampler, so recording currently produces silence regardless of this fix.
  Making the storage real-time-safe doesn't make recording functional;
  actually wiring live input into `FeedInputBlock()` is separate, deferred
  work (same reasoning as items 1/2 - no hardware here to verify audio-input
  correctness).

- `firmware/daisy/src/cv_bus.hpp` (the `CvBus` class) — superseded by the CV
  backend seam above. Its `Flush()` had a real bug: hardcoded to always
  flush DAC slot 0 regardless of which voice/group was queued (harmless in
  practice only because nothing ever called `QueueVoice`/`Flush` on it —
  `s_cv.Init()` was the only call site anywhere in the tree). The same
  calibration math and MCP4728 fast-write protocol now live correctly in
  `Mcp4728Backend`.

### Changed (ESP32 UI) — needs hardware verification

- Upgraded LVGL 9.3.0 → 9.5.0 and `esp_lvgl_port` to 2.8.0 (roadmap Phase
  0.1): pinned `lvgl/lvgl: '>=9.4,<10'` in `idf_component.yml` so the
  component manager can't silently resolve back down. Enabled
  `CONFIG_LVGL_PORT_ENABLE_PPA=y` (in both `sdkconfig` and
  `sdkconfig.defaults`) to offload display rotation to the ESP32-P4's PPA
  hardware instead of software. **Important correction to the original
  roadmap risk assessment**: our UI does use display rotation
  (`display_manager.cpp` sets `LV_DISPLAY_ROTATION_90`) — the roadmap had
  assumed otherwise when calling this low-risk, and `LVGL_PORT_ENABLE_PPA`'s
  Kconfig help text is literally "Enable PPA for screen rotation," so this
  is likely the correct fix but also puts us squarely in the "known PPA
  rotation-mode bugs on P4" risk category the roadmap flagged. **Only
  compile/build-verified so far** (full ESP32 build green in the
  devcontainer) — this needs to be flashed to the real HX8394/MIPI-DSI panel
  and checked for tearing/corruption during rotation before being trusted,
  and the FPS/CPU before-after profiling the roadmap called for on the
  waveform-preview and meter pages hasn't been done. Flip
  `CONFIG_LVGL_PORT_ENABLE_PPA` back to unset in both sdkconfig files if it
  misbehaves on hardware.

### Fixed (ESP32 dispatch)

- **UART `PacketRouter` injection was silently broken** (roadmap Phase 0.2
  item 4): `application_context.cpp` had the injection call commented out
  with "function not linking properly." Root cause was a namespace-scoping
  bug — the hand-rolled `extern` declaration was lexically inside
  `namespace WaveX`, so it declared (and looked up) `WaveX::
  uart_link_set_packet_router` instead of the real global-scope function
  defined in `esp_uart_link.cpp`. Fixed by including the real header and
  qualifying the call with `::`. Production UART traffic now routes through
  `ApplicationContext`'s single owned `PacketRouter` instead of a throwaway
  `dummy_router` fallback instance in `esp_uart_link.cpp`.

### Removed (ESP32 dispatch consolidation)

- **One event-dispatch owner on ESP32** (roadmap Phase 0.2 item 4):
  `PacketRouter` now owns fan-out; `inter_mcu` is the thin facade over
  `StatisticsManager` it was meant to be. Deleted: `ListenersManager`
  (`comm/listeners.h/.cpp`, wholesale dead — never instantiated anywhere);
  `inter_mcu_set_meter_listener`/`_set_browse_resp_listener` (dead redundant
  entry points — the live registration path is `CommInterfaceImpl` →
  `StatisticsManager` directly, bypassing these); the `s_sample_status_listener`
  static and its always-unreachable fallback branch in
  `inter_mcu_invoke_sample_status_callback` (`s_statistics` always wins in
  production); two dead duplicate SPI dispatch functions in `esp_spi_link.cpp`
  (`handle_control_message_from_daisy`, `_new_format`, zero callers) and the
  hand-rolled byte-unpacking `inter_mcu_process_daisy_control_message` they
  alone called (duplicated what `PacketRouter::handle_meter_push`/
  `handle_heartbeat` already do with typed structs). Left alone:
  `inter_mcu_set_wave_chunk_listener`/`_set_sample_status_listener` — both are
  genuinely live, used by production UI code, and `ICommInterface` has no
  wave-chunk equivalent to consolidate onto yet.

### Changed

- Upgraded `libDaisy` submodule v8.0.0 → v8.1.0 (roadmap Phase 0.1). Pulls in
  the `volatile`/error-flag fix for the SD DMA wait-loop compiler-optimization
  hazard and `HAL_SD_ErrorCallback` wiring, plus a reworked `WavPlayer` (unused
  by our custom streaming path). Local build-fix patches (44.1kHz SAI support,
  ADC HAL sources disabled, missing `stdint.h` include in the nested HAL
  driver) were rebased onto the new tag with no conflicts. `make daisy` and
  `make test` (79/79) pass; on-hardware SD soak test still needed before
  calling the Phase 0 gate closed.

- ESP32 partition table now uses the full 16 MB flash (roadmap Phase 0.2):
  factory image + two 4 MB OTA app slots with `otadata`, `userdata` grown from
  192 KB to ~3.9 MB, and the vestigial `samples` partition removed (samples
  live on the Daisy's SD card; no code referenced the partition). The app
  offset moved 0x10000 → 0x20000 — reflash via `flash-esp32.sh`/`idf.py
  flash` as usual; NVS offset/size are unchanged so stored settings survive.

### Removed

- Dead legacy SPI-based SD card backend: `sd_spi.cpp`/`sd_spi.h`/
  `diskio_sd_spi.cpp` (roadmap Phase 0.2). `WAVEX_DAISY_SD_CARD_BACKEND`
  defaults to `1` (SDIO) and nothing in the tree set it to `0`, so this path
  was unreachable. `esp_uart_link`/`daisy_uart_link` were left in place —
  despite the roadmap calling them legacy, they're the live transport for
  heartbeat/meter/status/ACK messages alongside SPI; `docs/roadmap.md` has
  been corrected.

### Fixed

- Test-build brittleness (roadmap Phase 0.2): GoogleTest is now a single
  vendored submodule (`firmware/shared/tests/_deps/googletest-src`, pinned
  `release-1.12.1`) referenced by all three test suites via
  `FetchContent_Declare(... SOURCE_DIR ...)`, so `make test` no longer needs
  network access. Removed a malformed duplicate nested submodule entry
  (`.../tests/_deps/firmware/daisy/tests/_deps/googletest-src`) left over from
  a `git submodule add` run in the wrong directory. `make test-daisy`/
  `test-esp32`/`test-shared` now wipe their build dirs before each run instead
  of reusing stale ones.
- The top-level `Makefile` and `firmware/daisy/Makefile` — the build
  entrypoints documented in `AGENTS.md` — were never in git: `.gitignore`'s
  bare `Makefile` pattern (meant for CMake-generated makefiles) was swallowing
  them. Added negation exceptions and tracked both files.
- **`ProtocolHandler::ParseWaveXPacket` buffer overflow** (roadmap Phase 0.2,
  found while writing exhaustive round-trip tests): the function ignored the
  caller-supplied destination capacity and always copied the packet's full
  zero-padded payload region, which is rounded up to the next size class (32/
  64/128/...). Any message struct smaller than its packet's padded region
  (e.g. `ErrorMessage`, `SampleLoadMessage`, `SampleMemStatusMessage`,
  `BrowseRespMessage`'s entries buffer) could be overrun by the extra padding
  bytes — reproducible as `*** stack smashing detected ***` once round-trip
  tests actually parsed those types back. Also fixed the one real firmware
  caller (`firmware/esp32/main/comm/packet_router.cpp`), which read the
  `payload_size` in/out parameter uninitialized. `payload_size` is now
  correctly treated as an in/out capacity: at most that many bytes are
  copied, and the true byte count copied is returned.

### Changed (protocol)

- **Wire-struct hygiene** (roadmap Phase 0.2): every message struct in
  `firmware/shared/spi_protocol/protocol.h` now has a zero-initializing
  default constructor and a named-argument constructor, and no other
  constructors — this makes each type a non-aggregate, so
  `Type x = {a, b, c};` / designated-initializer construction is now a
  **compile error** instead of a style guideline (`docs/features/inter-mcu-
  protocol.md` already warned about field-order bugs from aggregate init;
  now the compiler enforces it). All ~40 call sites across app and test code
  were migrated to `Type x(a, b, c);`. `SampleMemStatusMessage` gained a
  bounds-checked `AddEntry()` for its fixed `entries[]` array.
- Round-trip tests in `firmware/shared/tests/protocol/message_types_test.cpp`
  are now exhaustive: every message type is both created and parsed back
  with field-level assertions (several were previously create-only), and the
  four types with no coverage at all (`DataRequestMessage`,
  `StatusRequestMessage`, `SampleLoadMessage`, `SampleMemStatusMessage`/
  `SampleMemEntryMessage`) now have tests. This is what surfaced the
  `ParseWaveXPacket` overflow above.

## [0.1.0] - 2026-07-02

### Added

- Initial versioned baseline. Dual-MCU sampler/groovebox: ESP32-P4 frontend
  (LVGL touchscreen UI, encoders, MIDI I/O) and Daisy Seed backend (real-time
  audio engine, SD sample streaming, CV outputs), linked over SPI with a
  shared wire protocol.

[Unreleased]: https://github.com/maxamplitude/WaveX/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/maxamplitude/WaveX/releases/tag/v0.1.0
