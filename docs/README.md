# WaveX Documentation

Start here. Documents are grouped by whether they describe the system **as it should be built** (canonical), **how to work on it** (guides), or **history** (archive).

## Canonical design documents

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | **Single source of truth** for system design: product vision, hardware, firmware structure, real-time/DMA/timing rules, memory layout, open decisions |
| [roadmap.md](roadmap.md) | Implementation order (Phases 0–5), library upgrade recommendations (libDaisy, CMSIS-DSP, ESP-IDF) with risk callouts, per-phase test gates |
| [features/inter-mcu-protocol.md](features/inter-mcu-protocol.md) | As-built SPI wire specification (mirrors `firmware/shared/spi_protocol/protocol.h`) |
| [features/sequencer.md](features/sequencer.md) | Groovebox sequencer engine design (Phase 2) |
| [features/analog-voice-board.md](features/analog-voice-board.md) | PCM1690 TDM-8, per-voice VCF/VCA, CV calibration (Phase 3) |
| [features/offline-sample-editing.md](features/offline-sample-editing.md) | Offline render-job design for sample editing/mangling DSP (Phase 4) |

**Pin assignments and hardware flags are never documented in prose** — they live in `firmware/shared/config/pin_config.h` and `hardware_config.h` only.

## Working guides

| Document | Contents |
|---|---|
| [ui-architecture.md](ui-architecture.md) | ESP32 UI framework: navigator, pages, softkeys, LVGL threading rules |
| [ui-system-implementation-guide.md](ui-system-implementation-guide.md) | How to build a new UI page |
| [testing_guide.md](testing_guide.md) | Running and writing host tests (GoogleTest) |
| [performance_monitoring.md](performance_monitoring.md) | DWT cycle-counter / CPU-load measurement reference for the Daisy |
| [LICENSES.md](LICENSES.md) | Third-party license inventory |

## Archive (`archive/`)

Superseded or historical documents, kept for context. **Do not implement from these.** Notable entries:

- `system-architecture.md`, `communication-protocol.md`, `daisy_devel.md` — earlier architecture/protocol descriptions; contained mutually contradictory hardware claims (ESP32-S3 vs P4, UART vs SPI link, conflicting pin tables, MCP4728 vs MCP48CMB28). Replaced by `architecture.md` + `features/`.
- `testing_strategy.md` — a "test plan and audit report" containing results tables for tests that were never run and hardware we don't have (FT6336x touch, PEC11R encoders). Retained only as a checklist idea source.
- `ARCHITECTURE_ASSESSMENT_20260626.md` — external code assessment (June 2026); its recommendations are folded into `roadmap.md` Phase 0.
- `esp32-restart-hang-fix.md` — root-cause writeup from the retired UART link era; the lesson (bounded blocking + queue recovery) is now a rule in `architecture.md` §7.
- `daisy_spi_link_splitup_plan.md` — still-valid refactor plan for `daisy_spi_link.cpp`, scheduled opportunistically in roadmap Phase 0.5.
- UI docs (`UI_system.md`, `esp32_ui_page-based_navigation.md`, `navigation-integration-guide.md`) — superseded by `ui-architecture.md`.
- `encoder-implementation-plan.md`, `sample-browser-redesign.md` — implemented plans.

## Housekeeping notes

- `docs/venv/` is a stray Python virtualenv (used by `docx2md.py`); it should be removed or gitignored.
