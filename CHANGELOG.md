# Changelog

All notable changes to WaveX are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Frontend (ESP32-P4) and backend (Daisy Seed) firmware share a single version
number, defined in the root [`VERSION`](VERSION) file. See `AGENTS.md` for the
versioning and release process.

## [Unreleased]

### Added

- Synced versioning: a single root `VERSION` file now drives both the ESP32
  (`PROJECT_VER`) and Daisy (`project(... VERSION ...)`) firmware builds.
- `AGENTS.md` / `CLAUDE.md` project instructions.

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
