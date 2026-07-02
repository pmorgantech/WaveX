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

## [0.1.0] - 2026-07-02

### Added

- Initial versioned baseline. Dual-MCU sampler/groovebox: ESP32-P4 frontend
  (LVGL touchscreen UI, encoders, MIDI I/O) and Daisy Seed backend (real-time
  audio engine, SD sample streaming, CV outputs), linked over SPI with a
  shared wire protocol.

[Unreleased]: https://github.com/maxamplitude/WaveX/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/maxamplitude/WaveX/releases/tag/v0.1.0
