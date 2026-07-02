# WaveX — Agent Instructions

WaveX is a dual-MCU sampler/groovebox: **ESP32-P4** frontend (ESP-IDF 5.5, LVGL 9, touchscreen UI) and **Daisy Seed / STM32H750** backend (libDaisy, real-time audio engine). Read `README.md` for the project map before touching code.

## Orientation — read before designing or implementing

1. **`docs/roadmap.md`** — canonical implementation order (Phases 0–5), current phase, and the test gate for each phase. Always check which phase is active before proposing new work; do not jump ahead of the gate.
2. **`docs/architecture.md`** — single source of truth for system design: product vision, hardware split, real-time/DMA/cache rules (§7), memory layout, open decisions. When code and this doc disagree, code wins for *as-built* sections, the doc wins for *target design* sections (each section is labeled).
3. **`docs/features/*.md`** — as-built or target design for specific subsystems (inter-MCU protocol, sequencer, analog voice board, offline sample editing).
4. **`docs/ui-architecture.md`**, **`docs/ui-system-implementation-guide.md`**, **`docs/testing_guide.md`**, **`docs/performance_monitoring.md`** — working guides for UI, testing, and profiling.
5. **Never read or implement from `docs/archive/`.** Those documents are superseded, contain mutually contradictory hardware claims, or describe tests that were never run. They exist for historical context only.
6. **Pin assignments and hardware feature flags are never in prose docs.** They live exclusively in `firmware/shared/config/pin_config.h` and `firmware/shared/config/hardware_config.h`. Do not trust pin tables in commit history or archived docs.

If a task isn't clearly covered by the current roadmap phase or architecture doc, say so and propose where it fits rather than improvising a design.

## Non-negotiable engineering constraints

1. **The audio callback never blocks.** No SD I/O, no heap allocation, no I2C/SPI transactions, no logging inside the callback.
2. **All bulk data movement is DMA-driven** (audio SAI, SDMMC, inter-MCU SPI, MIPI-DSI) with explicit cache/alignment discipline — see `docs/architecture.md` §7 before writing any DMA-adjacent code.
3. **Sample editing/mangling is offline only.** Any DSP that cannot be guaranteed to finish within the audio block budget runs as a background render job (Daisy main loop), never in the real-time path.
4. **The inter-MCU wire contract is centralized** in `firmware/shared/spi_protocol/protocol.h`, mirrored in `docs/features/inter-mcu-protocol.md`, and covered by round-trip tests. Never hand-roll a competing message format.

## Audio/DSP performance discipline

- The real-time audio path (Daisy, 480 MHz Cortex-M7) is the one part of this codebase where micro-performance is a correctness property, not an optimization. A stable audio path with zero underruns is the product.
- Use **CMSIS-DSP** kernels (`arm_math.h`, e.g. `arm_linear_interp_q15`, biquad/FIR routines) wherever they cover the operation — do not hand-write scalar loops for filtering, interpolation, or FFT work the library already provides efficiently.
- Prefer fixed-point (q15) in the hot audio path where the existing engine already does; don't introduce float conversions in a loop that's currently integer unless profiling shows a real win.
- SDRAM access from the audio callback must be sequential/batched, never random single-word access (see architecture §7.1.3).
- Measure before/after with the DWT cycle counter (`docs/performance_monitoring.md`) for any change touching the audio callback, control tick, or a CMSIS-DSP kernel swap — don't assert a performance win without a number.
- Offline DSP (waveform mangling, time-stretch, pitch-shift, render jobs) still benefits from CMSIS-DSP, but has no hard real-time budget — correctness and audio quality take priority over cycle-shaving there.

## C++ conventions for this embedded target

- Target is bare-metal Cortex-M7 (Daisy) and FreeRTOS/ESP-IDF (ESP32-P4) — assume no OS heap guarantees on Daisy and a constrained one on ESP32.
- **Exceptions and RTTI are disabled on both targets** (`CONFIG_COMPILER_CXX_EXCEPTIONS`/`CXX_RTTI` off on ESP-IDF; equivalent on Daisy). Do not write code that throws, catches, or relies on `dynamic_cast`/`typeid`. Use return codes / status enums / `std::optional` for error signaling.
- No heap allocation in the audio callback or any interrupt context — this includes hidden allocations from `std::vector::push_back` past capacity, `std::string` growth, lambdas that capture by value into `std::function`, etc. Preallocate from the SDRAM allocator (`memory.h`) or use fixed-capacity containers.
- Avoid `<iostream>`/`<sstream>` and other heavyweight STL on-device; prefer the existing logging/metrics facilities.
- Templates and constexpr are fine and encouraged for zero-cost abstraction; avoid patterns that bloat code size unpredictably (heavy recursive template instantiation, excessive `std::variant`/`std::function` in hot paths) without checking the resulting binary size.
- Match the C++ standard already declared in each target's build files (see `firmware/daisy/tests/CMakeLists.txt`, ESP-IDF `sdkconfig`) — don't introduce a newer standard's features than what the toolchain for that MCU is configured to accept.
- Follow `.clang-format` for style; run pre-commit (`pre-commit install` per `README.md`) rather than hand-formatting.

## Versioning and changelog

- **Frontend (ESP32) and backend (Daisy) firmware share one version number**, defined in the single root **[`VERSION`](VERSION)** file (plain `MAJOR.MINOR.PATCH`, no prefix/suffix — required by CMake's `project(... VERSION ...)` parser). Both builds read it:
  - `firmware/daisy/CMakeLists.txt` reads `VERSION` into `project(wavex-daisy VERSION ...)`.
  - `firmware/esp32/CMakeLists.txt` reads `VERSION` into `PROJECT_VER` before the ESP-IDF `project()` call, which embeds it in `esp_app_desc_t`.
  - To bump the version, edit the root `VERSION` file only — never hardcode a version number in either CMakeLists.txt.
- This project follows **[Semantic Versioning](https://semver.org/)** (MAJOR.MINOR.PATCH). Given the project is pre-1.0 during active roadmap phases, breaking changes bump MINOR; treat 1.0.0 as the point where the wire protocol and on-disk formats (kits/patterns/songs, sidecar metadata) are considered stable.
- Maintain **`CHANGELOG.md`** at the repo root following **[Keep a Changelog](https://keepachangelog.com/)** conventions: an `[Unreleased]` section at the top, entries grouped under `Added` / `Changed` / `Fixed` / `Removed`, one dated version section per release.
- Every change with user- or developer-visible impact (new feature, protocol message, behavior change, bug fix) gets a changelog entry in the same commit/PR that makes the change. Pure refactors, formatting, and internal test-only changes don't require an entry.
- Do not bump the version number (the root `VERSION` file) as part of an unrelated change; version bumps are their own deliberate step tied to a release, done together with moving the corresponding `CHANGELOG.md` `[Unreleased]` entries into a new dated version section and tagging `vMAJOR.MINOR.PATCH`.
- `PROTOCOL_VERSION` in `firmware/shared/spi_protocol/protocol.h` is a separate concern — the SPI wire-format version, not the firmware release version. Don't conflate the two.

## Building and the devcontainer

- Full build/flash instructions (prerequisites, individual `idf.py`/`make` commands, debugging) live in **`setup.md`** and **`README.md`** — don't duplicate them here. Quick reference: `make all` (both MCUs), `make esp32` / `make daisy` (individually), `make test` (host tests).
- **Always build/test through the devcontainer** (`.devcontainer/Dockerfile` → image `wavex-devcontainer:latest`), not the host shell — the host toolchain is incomplete (no ESP-IDF env, `arm-none-eabi-gcc` present but the CMake toolchain file needs `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` set explicitly, which only the Daisy wrapper Makefile does correctly).
- The container runs as non-root user `petem` (uid 1000) by default, so bind-mounted build output is owned by the host user — no `chown` workaround needed after a CLI-driven build.
- For a one-off CLI build/test without opening VS Code's Dev Containers UI:
  ```bash
  docker build -t wavex-devcontainer:latest -f .devcontainer/Dockerfile .devcontainer/
  docker run --rm -v "$(pwd)":/workspaces/WaveX -w /workspaces/WaveX wavex-devcontainer:latest \
    bash -lc 'source /opt/esp/idf/export.sh && make all -j$(nproc) && make test'
  ```
  Mount at `/workspaces/WaveX` (matching what VS Code's Dev Containers extension uses) so `build/` CMake caches generated by either path stay compatible — a mismatched mount path leaves stale absolute paths in `CMakeCache.txt` that only surface as a confusing compiler-not-found error; `make clean` / `idf.py fullclean` recovers.
  `source /opt/esp/idf/export.sh` is required in a raw `docker run` even though `PATH`/`IDF_TOOLS_PATH` are already baked into the image, because it also activates the Python venv `idf.py` depends on; VS Code sessions get this for free via `postCreateCommand`.
- **Use `-j$(nproc)`** on `make` invocations (top-level `make all`/`make daisy`, or inside `firmware/daisy`) — GNU Make's jobserver propagates through the nested `$(MAKE)` calls in the Daisy wrapper Makefile automatically. `idf.py build` (ESP32) already parallelizes via ninja without needing an explicit flag.
- `build/` directories are gitignored; if compiles fail immediately with a compiler-not-found or path error, suspect a stale cache left by a build under a different mount path or an older toolchain version, and `make clean`/`idf.py fullclean` before re-investigating.

## Testing

- Host tests run via GoogleTest (`make test`); see `docs/testing_guide.md`. Every roadmap phase ends with a stated test gate (`docs/roadmap.md`) — don't consider a phase's work done until its gate is green.
- New wire-protocol messages require round-trip tests in `firmware/shared/tests/protocol/` before UI or engine work that depends on them.
- For hardware-dependent behavior that can't be host-tested (SD soak tests, CV calibration, scope-verified timing), state explicitly what was and wasn't verified rather than claiming untested behavior works.

## General discipline

- Prefer small, reviewable changes. Don't bundle unrelated refactors with feature work.
- Don't duplicate pin tables, wire-format tables, or architecture diagrams into new docs or comments — link to the single source of truth (`pin_config.h`, `hardware_config.h`, `protocol.h`, `architecture.md`) instead.
- If you find contradictory information between an archived doc and `architecture.md`/`roadmap.md`/code, trust the canonical doc or code, and flag the discrepancy rather than silently picking one.
