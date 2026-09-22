# WaveX Documentation

Everything here describes **what is** or **what will be**. Finished work lives
in [`CHANGELOG.md`](../CHANGELOG.md) and git history, not in this directory —
see `roadmap.md` § Rules for every phase for the rule and what happens to a doc
when it is superseded.

Start with `project-principles.md` (why the architecture exists),
`architecture.md` (how the system is built), and `roadmap.md` (what order it
gets built in). Everything else hangs off those three.

**Pin assignments and hardware flags are never documented in prose** — they
live in `firmware/shared/config/pin_config.h` and `hardware_config.h` only.

## Canonical

| Document | Contents |
|---|---|
| [project-principles.md](project-principles.md) | **Architectural constitution**: the 15 engineering principles and decision filter that every significant design, implementation, and review must satisfy |
| [architecture.md](architecture.md) | **Single source of truth** for system design: product vision, hardware, firmware structure, real-time/DMA/timing rules (§7), memory layout, open decisions |
| [roadmap.md](roadmap.md) | Implementation order (Phases 0–5), next steps/backlog and open decisions, per-phase test gates, and § Outstanding hardware verification — code-complete work that nothing has yet proven on the bench |
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
| [hybrid-inter-mcu-link.md](features/hybrid-inter-mcu-link.md) | Proposed; no hybrid firmware yet | UART controls and confirming replies, SPI waveform/meters, ownership, interval telemetry and recovery gates |
| [panel-controls.md](features/panel-controls.md) | Logical keys built; drivers partial | Panel key/LED model, input stages and MIDI hardware gates |
| [sequencer.md](features/sequencer.md) | Callback preview built; panel and Track integration open | Sequencer timing, digital audition, ownership and Phase 2 gaps |
| [track-and-patch-model.md](features/track-and-patch-model.md) | Confirmed vocabulary; decisions taken 2026-09-04 | Sample Pool → Oscillator → Instrument → Track → Pattern/Scene/Song hierarchy, the Bank, the two-oscillator Instrument, workflows, and save boundaries |
| [oscillator-sources.md](features/oscillator-sources.md) | Sampler boundary accepted; wavetable deferred | Typed Sample and Wavetable oscillator contracts behind the shared Instrument/Voice path |
| [instrument-model.md](features/instrument-model.md) | Sampler core and WXI read path built | Zones, note resolution, common Sample Pool ownership and persistence boundaries |
| [sfz-import.md](features/sfz-import.md) | Import path built; bench gate open | Loading third-party `.sfz` multisamples into the instrument model |
| [debug-harness-and-hil.md](features/debug-harness-and-hil.md) | Built (2026-09-04) | The acknowledged `WAVEX-DBG` console on both boards - input injection, synthetic touch, `STATE` queries, Daisy message injection - and the `make test-hil` suite in `tests/hil/` |
| [melodic-sequencing.md](features/melodic-sequencing.md) | Target design | Melodic track type, chords/ties, step-record, live record/overdub/erase (Phase 2.5) |
| [param-locks-and-modulation.md](features/param-locks-and-modulation.md) | Target design | Parameter locks, modulation matrix, LFOs, filter envelope (Phase 2/2.5) |
| [sampling-and-recording.md](features/sampling-and-recording.md) | Target design | Threshold-armed capture, pre-roll, resample/bounce, assign-to-zone (Phase 2.5) |
| [output-routing-and-mixer.md](features/output-routing-and-mixer.md) | Target design | Mixer v1: 16-track gain/pan/mute/solo, per-track meters (Phase 2.5) |
| [arpeggiator.md](features/arpeggiator.md) | Target design | Per-slot, clock-synced arpeggiator with latch (Phase 2.5) |
| [midi-sync-tempo-follower.md](features/midi-sync-tempo-follower.md) | Target design | MIDI clock in/out and tempo follower; required for the Phase 2 gate |
| [analog-voice-board.md](features/analog-voice-board.md) | Target design, Stage A buildable | PCM1690 TDM-8, per-voice VCF/VCA, CV calibration (Phase 3) |
| [offline-sample-editing.md](features/offline-sample-editing.md) | Target design | Offline render-job model for destructive editing and mangling DSP (Phase 4) |
| [vintage-sampler-math.md](features/vintage-sampler-math.md) | Research proposal | Fixed-48 kHz engine with virtual sample clocks, companding and reconstruction; numerical reference only |
| [virtual-analog-filters.md](features/virtual-analog-filters.md) | Research; comparisons unscheduled | Existing TPT SVF/ZDF ladder, nonlinear-solve and antialiasing tradeoffs, analog measurements and capacity-gated evaluation |
| [scenes-and-performance.md](features/scenes-and-performance.md) | Target design | Song mode/pattern chaining, performance macros, scenes with morph (Phase 5) |
| [tuning-and-scales.md](features/tuning-and-scales.md) | Target design | Master tune, 12-degree tables, scale-constrained input surfaces (Phase 5) |

## Working guides

| Document | Contents |
|---|---|
| [architecture-notes.md](architecture-notes.md) | Exploratory ASR-10 and E4/EOS comparisons: navigation, edit scope, multisample synthesis, modulation and loading; promotion stays in the roadmap backlog |
| [ui-architecture.md](ui-architecture.md) | ESP32 UI framework and navigation map: page lifecycle, softkeys, synchronized updates and page registration |
| [ui-design-constraints.md](ui-design-constraints.md) | One-page brief for UI/UX design passes — display, fonts, palette, rendering budget, widget inventory, each claim cited to code |
| [logging.md](logging.md) | Per-module log levels with compile-time ceilings and runtime control, on both consoles |
| [testing_guide.md](testing_guide.md) | Host tests, meaningful regressions, production coverage boundaries and open test work |
| [hardware-validation.md](hardware-validation.md) | Periodic bench checklist: pending/deferred physical checks, setup, pass criteria and recorded results |
| [flashing.md](flashing.md) | Build and flash the ESP32-P4 and Daisy Seed from the devcontainer, plus SWD/GDB debug-probe workflows |
| [performance_monitoring.md](performance_monitoring.md) | DWT cycle-counter and CPU-load measurement on the Daisy; LVGL render/flush/FPS instrumentation on the ESP32 |
| [firmware-size-log.md](firmware-size-log.md) | Measured firmware size by build and memory region |
| [callback-performance-log.md](callback-performance-log.md) | Durable target-hardware results from the recurring Daisy callback-headroom gate |
| [spi-notes.md](spi-notes.md) | UART/SPI measured loads and control latency, scheduling and return-path experiments, tested image identities, rollback state, and remaining SPI gates |
| [uart-baud-notes.md](uart-baud-notes.md) | Faster-UART comparison, eight sustained Instruments, callback/control measurements, and audible timing investigation |
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
still open has been rescued into `roadmap.md` or a
`features/*.md`. Git history is the archive: `git log --diff-filter=D --
docs/` lists what has left.
