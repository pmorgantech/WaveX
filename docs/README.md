# WaveX Documentation

Everything here describes **what is** or **what will be**. Finished work lives
in [`CHANGELOG.md`](../CHANGELOG.md) and git history, not in this directory —
see `roadmap.md` § Cross-Cutting Rules for the rule and what happens to a doc
when it is superseded.

Start with `architecture.md` (how the system is built) and `roadmap.md` (what
order it gets built in). Everything else hangs off those two.

**Pin assignments and hardware flags are never documented in prose** — they
live in `firmware/shared/config/pin_config.h` and `hardware_config.h` only.

## Canonical

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | **Single source of truth** for system design: product vision, hardware, firmware structure, real-time/DMA/timing rules (§7), memory layout, open decisions |
| [roadmap.md](roadmap.md) | Implementation order (Phases 0–5), per-phase test gates, library upgrade recommendations with risk callouts, and § Outstanding hardware verification — code-complete work that nothing has yet proven on the bench |
| [backlog.md](backlog.md) | Work worth doing but not scheduled into a phase. Every entry records **why it is not urgent**, so a future reader can tell whether the reasoning still holds |
| [rt1170-migration.md](rt1170-migration.md) | Forward-looking plan for moving the backend off the STM32H750 |

## Required reading before writing code

| Document | Applies to |
|---|---|
| [daisy_rt_audio_coding_guide.md](daisy_rt_audio_coding_guide.md) | Daisy Seed / STM32H750 / libDaisy / CMSIS-DSP real-time audio code. Load the `daisy` project skill alongside it |
| [esp32p4_coding_guide.md](esp32p4_coding_guide.md) | ESP32-P4 / ESP-IDF embedded code. Load the `esp32p4` project skill alongside it; §14 is the review checklist |

## Feature designs (`features/`)

Each carries an explicit status line saying what is built and what is not.
Per `roadmap.md`, a new subsystem gets its design doc here **before**
implementation.

| Document | Status | Contents |
|---|---|---|
| [inter-mcu-protocol.md](features/inter-mcu-protocol.md) | **As-built** | The live wire specification, mirroring `firmware/shared/spi_protocol/protocol.h`. If the two diverge, fix one in the same commit that changed the other |
| [digital-voice-audition.md](features/digital-voice-audition.md) | **Active work order** | Consolidated ordered path to a playable, sequenceable all-digital voice (Phase 2, borrowing narrowly from Phase 2.5) |
| [sequencer.md](features/sequencer.md) | Core built, integration open | Groovebox sequencer engine (Phase 2) |
| [instrument-model.md](features/instrument-model.md) | Core built, rest open | Presets, zones, multisampling, velocity layers — the Emax/Emulator lineage (Phase 2.5) |
| [sfz-import.md](features/sfz-import.md) | Proposed | Loading third-party `.sfz` multisamples into the instrument model |
| [melodic-sequencing.md](features/melodic-sequencing.md) | Target design | Melodic track type, chords/ties, step-record, live record/overdub/erase (Phase 2.5) |
| [param-locks-and-modulation.md](features/param-locks-and-modulation.md) | Target design | Parameter locks, modulation matrix, LFOs, filter envelope (Phase 2/2.5) |
| [sampling-and-recording.md](features/sampling-and-recording.md) | Target design | Threshold-armed capture, pre-roll, resample/bounce, assign-to-zone (Phase 2.5) |
| [output-routing-and-mixer.md](features/output-routing-and-mixer.md) | Target design | Mixer v1: 16-track gain/pan/mute/solo, per-track meters (Phase 2.5) |
| [arpeggiator.md](features/arpeggiator.md) | Target design | Per-slot, clock-synced arpeggiator with latch (Phase 2.5) |
| [midi-sync-tempo-follower.md](features/midi-sync-tempo-follower.md) | Target design | MIDI clock in/out and tempo follower; required for the Phase 2 gate |
| [analog-voice-board.md](features/analog-voice-board.md) | Target design, Stage A buildable | PCM1690 TDM-8, per-voice VCF/VCA, CV calibration (Phase 3) |
| [offline-sample-editing.md](features/offline-sample-editing.md) | Target design | Offline render-job model for destructive editing and mangling DSP (Phase 4) |
| [scenes-and-performance.md](features/scenes-and-performance.md) | Target design | Song mode/pattern chaining, performance macros, scenes with morph (Phase 5) |
| [tuning-and-scales.md](features/tuning-and-scales.md) | Target design | Master tune, 12-degree tables, scale-constrained input surfaces (Phase 5) |
| [feature-expansion-ideas.md](features/feature-expansion-ideas.md) | Index | Map over the feature-design suite, plus the **protocol message-ID reservation table** — check it before claiming an ID |

## Working guides

| Document | Contents |
|---|---|
| [ui-architecture.md](ui-architecture.md) | ESP32 UI framework: navigator, pages, softkeys, LVGL threading rules, and how to build and register a new page |
| [ui-information-architecture.md](ui-information-architecture.md) | Target menu structure: page/tab inventory, menu vs. tab group, screen-by-screen layout |
| [ui-design-constraints.md](ui-design-constraints.md) | One-page brief for UI/UX design passes — display, fonts, palette, rendering budget, widget inventory, each claim cited to code |
| [logging.md](logging.md) | Per-module log levels with compile-time ceilings and runtime control, on both consoles |
| [testing_guide.md](testing_guide.md) | Running and writing host tests (GoogleTest), and how to write a regression test that actually fails against the pre-fix code |
| [testing-remediation.md](testing-remediation.md) | Live plan for closing the gap between what the August 2026 audits found and what the suite can catch. Tier 0–1 largely done; Tiers 2–4 open |
| [flashing.md](flashing.md) | Build and flash the ESP32-P4 and Daisy Seed from the devcontainer, plus SWD/GDB debug-probe workflows |
| [performance_monitoring.md](performance_monitoring.md) | DWT cycle-counter and CPU-load measurement on the Daisy; LVGL render/flush/FPS instrumentation on the ESP32 |
| [LICENSES.md](LICENSES.md) | Third-party license inventory |

For the development quickstart, build commands and CI, see the
[project README](../README.md). For engineering constraints and conventions,
see [`AGENTS.md`](../AGENTS.md).

## A note on `archive/`

`docs/archive/` is **gitignored** — it is not part of the repository, and a
clone will not have it. It survives only on machines that once held those
files. Never read or implement from it: its contents are superseded, carry
mutually contradictory hardware claims (ESP32-S3 vs P4, UART vs SPI link,
conflicting pin tables), or report tests that were never run.

Superseded documents are now deleted rather than moved there, after anything
still open has been rescued into `roadmap.md`, `backlog.md` or a
`features/*.md`. Git history is the archive: `git log --diff-filter=D --
docs/` lists what has left.
