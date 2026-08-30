# WaveX Documentation

Start here. Documents are grouped by whether they describe the system **as it should be built** (canonical), **how to work on it** (guides), or **history** (archive).

## Canonical design documents

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | **Single source of truth** for system design: product vision, hardware, firmware structure, real-time/DMA/timing rules, memory layout, open decisions |
| [backlog.md](backlog.md) | Unscheduled work, each entry recording why it is not urgent so the reasoning can be re-checked later |
| [roadmap.md](roadmap.md) | Implementation order (Phases 0–5), library upgrade recommendations (libDaisy, CMSIS-DSP, ESP-IDF) with risk callouts, per-phase test gates |
| [features/inter-mcu-protocol.md](features/inter-mcu-protocol.md) | As-built SPI wire specification (mirrors `firmware/shared/spi_protocol/protocol.h`) |
| [features/digital-voice-audition.md](features/digital-voice-audition.md) | **Active work order.** Consolidated ordered path to a playable, sequenceable all-digital voice (Phase 2, borrowing narrowly from Phase 2.5) |
| [features/sequencer.md](features/sequencer.md) | Groovebox sequencer engine design (Phase 2) |
| [features/instrument-model.md](features/instrument-model.md) | Presets, zones, multisampling, velocity layers — the Emax/Emulator lineage (Phase 2.5) |
| [features/melodic-sequencing.md](features/melodic-sequencing.md) | Melodic track type, chords/ties, step-record, live record/overdub/erase (Phase 2.5) |
| [features/param-locks-and-modulation.md](features/param-locks-and-modulation.md) | Parameter locks, modulation matrix, LFOs, filter envelope (Phase 2.5) |
| [features/sampling-and-recording.md](features/sampling-and-recording.md) | Threshold-armed capture, pre-roll, resample/bounce, assign-to-zone (Phase 2.5) |
| [features/output-routing-and-mixer.md](features/output-routing-and-mixer.md) | Mixer v1: 16-track gain/pan/mute/solo, per-track meters (Phase 2.5) |
| [features/arpeggiator.md](features/arpeggiator.md) | Per-slot, clock-synced arpeggiator with latch (Phase 2.5) |
| [features/midi-sync-tempo-follower.md](features/midi-sync-tempo-follower.md) | MIDI clock in/out and tempo-follower design |
| [features/analog-voice-board.md](features/analog-voice-board.md) | PCM1690 TDM-8, per-voice VCF/VCA, CV calibration (Phase 3) |
| [features/offline-sample-editing.md](features/offline-sample-editing.md) | Offline render-job design for sample editing/mangling DSP (Phase 4) |
| [features/scenes-and-performance.md](features/scenes-and-performance.md) | Song mode/pattern chaining, performance macros, scenes with morph (Phase 5) |
| [features/tuning-and-scales.md](features/tuning-and-scales.md) | Master tune, 12-degree tables, scale-constrained input surfaces (Phase 5) |
| [features/feature-expansion-ideas.md](features/feature-expansion-ideas.md) | Index/rationale for the 2026-07-05 feature-design suite, plus the protocol message-ID reservation table |
| [daisy_rt_audio_coding_guide.md](daisy_rt_audio_coding_guide.md) | Required guidance for Daisy Seed / STM32H750 / libDaisy / CMSIS-DSP real-time audio code |
| [esp32p4_coding_guide.md](esp32p4_coding_guide.md) | Required guidance for ESP32-P4 / ESP-IDF embedded code |

**Pin assignments and hardware flags are never documented in prose** — they live in `firmware/shared/config/pin_config.h` and `hardware_config.h` only.

## Working guides

| Document | Contents |
|---|---|
| [ui-diagnostics-spec.md](ui-diagnostics-spec.md) | What the diagnostics screen should show, why each figure earns its place, and the `MSG_DIAG_PUSH` addition it needs |
| [ui-design-constraints.md](ui-design-constraints.md) | One-page brief for UI/UX design passes: display, fonts, palette, rendering budget, widget inventory — each claim with its source in code |
| [ui-architecture.md](ui-architecture.md) | ESP32 UI framework: navigator, pages, softkeys, LVGL threading rules |
| [ui-information-architecture.md](ui-information-architecture.md) | Page/tab inventory and navigation structure: menu vs. tab group, current screen-by-screen layout |
| [ui-system-implementation-guide.md](ui-system-implementation-guide.md) | How to build a new UI page |
| [ui-update-backlog.md](ui-update-backlog.md) | Tracked UI fixes found by reading code (tab tracking, edit-page threading, CPU tiles, etc.) — see `roadmap.md` § Outstanding hardware verification |
| [testing_guide.md](testing_guide.md) | Running and writing host tests (GoogleTest) |
| [flashing.md](flashing.md) | Build and flash the ESP32-P4 and Daisy Seed firmware from the devcontainer |
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

## Status reports

Dated snapshots of implementation progress against `roadmap.md`, not living documents — check the date before trusting anything in one over the canonical docs above.

| Document | Contents |
|---|---|
| [status-2026-07-02.md](status-2026-07-02.md) | Phase 0 (Foundation Hardening) complete + Phase 1 items 1–3 (output/CV seam, RAM-resident voice manager, real-time-safe recording buffer); bugs found along the way; what still needs hardware verification |
| [dma-timing-review-2026-07-03.md](dma-timing-review-2026-07-03.md) | DMA/timing code review of the UART link + audio path: 12 findings (3 high: 918.75 Hz control tick vs the 1 kHz invariant → **48 kHz decision recorded**, oversize-frame transmit wedge, UART-over-audio priority inversion); fix order lives in `roadmap.md` Phase 1 |
| [code_review_20260705.md](code_review_20260705.md) | Full first-party tree review (shared + Daisy + ESP32): broken MIDI note dispatch, inert legacy DSP surface, inverted send-error results, transport docs backwards; several items since fixed (see the 2026-08-29 review §8 for ESP32 status) |
| [field-findings-20260829.md](field-findings-20260829.md) | First bench session after the ESP32 remediation: Daisy CPU load ~2x on playback, keyboard silent with no diagnosable output, sample-edit auditions the wrong sample so no edit is audible, no sample management, no Voice/Preset entity. Sets the work order |
| [code_review_esp32_20260829.md](code_review_esp32_20260829.md) | ESP32-P4-only review against `docs/esp32p4_coding_guide.md`: ~40 tracked findings with checkboxes (LVGL locking violations, callback-lifetime UAFs, nonfunctional keypad decode, cross-core encoder race, inert CMake options) + SPI-link revival gate (§7) |

## Housekeeping notes

- `docs/venv/` is a stray Python virtualenv (used by `docx2md.py`); already gitignored (`.gitignore`), but still present on disk here — safe to `rm -rf docs/venv` locally.
