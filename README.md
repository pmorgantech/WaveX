# WaveX — Dual-MCU Sampler / Groovebox

WaveX is a modern **sampler / groovebox / drum machine** with a 5" touchscreen, per-voice **analog filtering** (VCF/VCA), CV/Gate outputs, and offline sample editing — built on a dual-MCU architecture:

- **Frontend — ESP32-P4** (ESP-IDF 5.5, LVGL 9): 1280×720 MIPI-DSI touchscreen UI, encoders, button matrix, LEDs, MIDI I/O.
- **Backend — Daisy Seed / STM32H750** (libDaisy, 480 MHz Cortex-M7, 64 MB SDRAM): real-time audio engine at 48 kHz, sample streaming from SD (SDMMC 4-bit), CV outputs, DSP.
- **Inter-MCU link**: UART at 2 Mbaud (ESP32 UART1 ↔ Daisy UART4), full-duplex DMA both ends, carrying a shared, tested wire protocol (`firmware/shared/uart_protocol/` framing over `firmware/shared/spi_protocol/protocol.h` payloads) with CRC16 and sequence numbers. An SPI link is wired and an ATTN line exists, but both are **compiled out** (`WAVEX_SPI_LINK_ENABLED=0`) — see [`docs/architecture.md`](docs/architecture.md) §4.4.

## Documentation

**Start with [`docs/README.md`](docs/README.md)** — it indexes everything and says what each document is for. The three that matter most:

- [`docs/architecture.md`](docs/architecture.md) — canonical system design, including the real-time / DMA / cache rules all code must follow.
- [`docs/roadmap.md`](docs/roadmap.md) — implementation order, per-phase test gates, and the hardware verification still outstanding.
- [`docs/backlog.md`](docs/backlog.md) — concise unscheduled work and open decisions.

The docs describe what is and what will be; finished work lives in [`CHANGELOG.md`](CHANGELOG.md) and git history.

**Hardware pins and feature flags are defined in code, not docs**: `firmware/shared/config/pin_config.h` (all pins, both MCUs) and `firmware/shared/config/hardware_config.h` (peripheral flags). Do not trust pin tables found in older documents or commit history — they went through several contradictory revisions; `pin_config.h` is the single source of truth.

## Project Structure

```
WaveX/
├── firmware/
│   ├── esp32/                 # ESP32-P4 frontend (ESP-IDF, idf.py/CMake)
│   │   ├── main/              # app entry, tasks, inter-MCU client, links/
│   │   ├── components/ui/     # LVGL navigator/page/softkey UI framework
│   │   └── managed_components/# lvgl, esp_lvgl_port, display/touch drivers
│   ├── daisy/                 # Daisy Seed backend (CMake + arm-none-eabi)
│   │   ├── src/               # audio/, comm/, storage/, metrics/, profiling/
│   │   └── libs/              # libDaisy, DaisySP (submodules)
│   └── shared/
│       ├── uart_protocol/     # live link framing (markers, CRC16, sequence)
│       ├── spi_protocol/      # inter-MCU payload catalog (protocol.h) + impl
│       ├── wav/ wxcf/ midi/   # container parsing and MIDI types
│       └── config/            # pin_config.h, hardware_config.h, link + logging config
├── docs/                      # see docs/README.md
├── tools/                     # ui_preview, docx2md.py
├── scripts/                   # serial port resolution, DFU trigger, sysmon, log config
├── build.sh / Makefile        # top-level build orchestration
└── flash-esp32.sh / monitor-esp32.sh
```

## Development Setup

### Prerequisites

- **Python 3.10+**, **Git** (with submodules), **VS Code** with Dev Containers extension (recommended)

### Quickstart (devcontainer — recommended)

1. Clone with submodules:
   ```bash
   git clone --recursive <repo-url> && cd WaveX
   ```
2. Open in VS Code → F1 → "Reopen in Container" (or `./devcontainer.sh` for a CLI shell).
   Either entry point points `core.hooksPath` at the tracked `.githooks/`
   directory automatically — do **not** run `pre-commit install`.
3. Build everything: `make all`
4. Flash persistently: `make esp32-flash` (or `./flash-esp32.sh`); Daisy via
   QSPI/DFU (`make daisy-flash`, hold BOOT while resetting). For the faster,
   volatile Daisy edit/test loop with an ST-Link, use `make daisy-debug`.

**All work happens inside the devcontainer — including `git commit`.** The
pre-commit suite (formatting, firmware builds, host tests) only exists in the
container image, and the tracked hook wrapper (`.githooks/pre-commit`) blocks
commits made from the host with instructions to re-run from the container.
Emergency escape hatch: `WAVEX_ALLOW_HOST_COMMIT=1 git commit ...` commits
with **no checks at all** — use sparingly.

See [`docs/flashing.md`](docs/flashing.md) for detailed flash and debug-probe workflows.

### Build & test commands

```bash
make all           # Build ESP32 + Daisy (debug profile)
make esp32         # ESP32 frontend only
make daisy         # Daisy backend only
make daisy-debug   # Build + load the Daisy SRAM image over ST-Link + run
make daisy-debug-build  # Build the separate SRAM/debug ELF without loading it
make clean         # Clean both
make setup         # Init git submodules

make test          # All host unit tests (GoogleTest)
make test-shared   # Shared protocol tests
make test-esp32    # ESP32 component tests
make test-daisy    # Daisy component tests
make test-clean    # Clean test build artifacts

make release       # Both MCUs, release profile, then verify the token gate
make check-profiles # Assert tokens present in debug AND absent in release
```

### Build profiles

Builds use the **debug profile** by default. It includes runtime log-level
control on both boards and ESP32 serial screenshots. Build the production
profile with:

```bash
make release        # Both boards, then check that debug tokens are absent
make daisy-release  # Daisy only
make esp32-release  # ESP32 only
```

Release builds set `WAVEX_BUILD_DEBUG=0`, which compiles out the debug console
surface and ESP32 screenshots. Each profile uses its own `build-release/`
directory. Do not reuse a build directory for a different profile: the Daisy
wrapper does not reconfigure an existing CMake cache, so it can silently retain
the previous profile's flags.

The Daisy also has a separate SRAM-linked debug profile in `build-debug/` for
fast SWD iteration. `make daisy-debug` loads that ELF directly through ST-Link
without changing QSPI; a reset or power cycle returns to the persistent QSPI
image. It builds at `-O0` (`DEBUG_OPT`) for stepping, whereas the persistent
image is `-O2` (`WAVEX_DAISY_OPT`, `firmware/daisy/CMakeLists.txt`), and SRAM
execution has different memory timing; use it for functional testing and the
normal QSPI build for DWT/callback performance results.

`WAVEX-ENTER-DFU` deliberately remains in release images; it is the only
reflash path that does not require BOOT+RESET. `make check-profiles` requires
both debug and release images and confirms the console tokens exist only in the
debug images.

The ESP32 side builds with ESP-IDF's `idf.py` (component-based); the Daisy side
uses CMake with the libDaisy toolchain file. Its default/persistent app is placed
in QSPI through the Daisy bootloader; its separate fast-debug app runs from
internal SRAM after a direct SWD load. Testing details:
[`docs/testing_guide.md`](docs/testing_guide.md).

## CI and Code Quality

- **GitHub Actions** on pushes/PRs to `main`/`develop`: unit tests (shared protocol, ESP32, Daisy) + build verification for both firmwares, with artifact caching.
- **Pre-commit hooks** handle formatting and verification: clang-format (Google style, 4-space indent, 100 col), black + isort, prettier for YAML, whitespace/EOF/merge-conflict/large-file checks, plus firmware builds and host test runs. They run **only inside the devcontainer** (see Quick start above); commits from the host are blocked by `.githooks/pre-commit`.

```bash
# All from a devcontainer shell:
pre-commit run            # staged files
pre-commit run --all-files
pre-commit autoupdate
```

## Hardware Summary

Full component table and open hardware decisions: [`docs/architecture.md`](docs/architecture.md) §3.

| | Frontend | Backend |
|---|---|---|
| MCU | ESP32-P4 (Waveshare P4-WIFI6 board, 16 MB flash, PSRAM) | Daisy Seed (STM32H750, 64 MB SDRAM, 8 MB QSPI) |
| Display/UI | 5" 1280×720 MIPI-DSI (HX8394) + GT911 touch, PCNT encoder, TCA8418 button matrix, TLC5947 LEDs | — |
| Audio | — | built-in stereo codec (SAI1); PCM1690 8-ch TDM DAC planned (SAI2) |
| Storage | 16 MB flash | microSD via SDMMC 4-bit + FatFs |
| MIDI | DIN (UART2) + USB | — |
| CV/Gate | — | CV DAC bus (part selection in progress — see architecture §3.3) |
| Link | UART1 (2 Mbaud, DMA) | UART4 (2 Mbaud, DMA) — SPI + ATTN wired but compiled out |

Note: the ESP32-P4 itself has **no radio**; the board's WiFi 6 comes from an onboard ESP32-C6 (currently unused).

## Contributing

1. Fork, branch (`git checkout -b feature/amazing-feature`)
2. Follow `docs/architecture.md` §7 (DMA/cache/timing rules) and the wire-contract rules in `docs/features/inter-mcu-protocol.md`
3. Keep `make test` green; add round-trip tests for any protocol change
4. Open a Pull Request

## License

MIT — see [LICENSE](LICENSE). Third-party licenses: [`docs/LICENSES.md`](docs/LICENSES.md).

## Acknowledgments

- [Electro-Smith](https://www.electro-smith.com/) — Daisy platform, libDaisy, DaisySP
- [LVGL](https://lvgl.io/) — embedded graphics
- [Espressif](https://www.espressif.com/) — ESP32-P4, ESP-IDF
