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

## [0.1.0] - 2026-07-02

### Added

- Initial versioned baseline. Dual-MCU sampler/groovebox: ESP32-P4 frontend
  (LVGL touchscreen UI, encoders, MIDI I/O) and Daisy Seed backend (real-time
  audio engine, SD sample streaming, CV outputs), linked over SPI with a
  shared wire protocol.

[Unreleased]: https://github.com/maxamplitude/WaveX/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/maxamplitude/WaveX/releases/tag/v0.1.0
