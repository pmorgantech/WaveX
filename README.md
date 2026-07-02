# WaveX — Dual-MCU Sampler / Groovebox

WaveX is a modern **sampler / groovebox / drum machine** with a 5" touchscreen, per-voice **analog filtering** (VCF/VCA), CV/Gate outputs, and offline sample editing — built on a dual-MCU architecture:

- **Frontend — ESP32-P4** (ESP-IDF 5.5, LVGL 9): 1280×720 MIPI-DSI touchscreen UI, encoders, button matrix, LEDs, MIDI I/O.
- **Backend — Daisy Seed / STM32H750** (libDaisy, 480 MHz Cortex-M7, 64 MB SDRAM): real-time audio engine at 48 kHz, sample streaming from SD (SDMMC 4-bit), CV outputs, DSP.
- **Inter-MCU link**: SPI (Daisy master, ESP32 slave) with a shared, tested wire protocol (`firmware/shared/spi_protocol/`), CRC16, sequence numbers, and an attention line for slave-initiated data.

## Documentation

**Start with [`docs/README.md`](docs/README.md).** The two documents that matter most:

- [`docs/architecture.md`](docs/architecture.md) — canonical system design, including the real-time / DMA / cache rules all code must follow.
- [`docs/roadmap.md`](docs/roadmap.md) — implementation order and library upgrade plan.

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
│       ├── spi_protocol/      # inter-MCU wire contract (protocol.h) + impl
│       └── config/            # pin_config.h, hardware_config.h, link config
├── docs/                      # see docs/README.md
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
2. Open in VS Code → F1 → "Reopen in Container".
3. Set up hooks:
   ```bash
   source .env/bin/activate
   pre-commit install
   ```
4. Build everything: `make all`
5. Flash: `make esp32-flash` (or `./flash-esp32.sh`); Daisy via DFU (`make -C firmware/daisy flash`, hold BOOT on power-up).

See [`setup.md`](setup.md) for local (non-container) setup and detailed flash/debug workflows.

### Build & test commands

```bash
make all           # Build ESP32 + Daisy
make esp32         # ESP32 frontend only
make daisy         # Daisy backend only
make clean         # Clean both
make setup         # Init git submodules

make test          # All host unit tests (GoogleTest)
make test-shared   # Shared protocol tests
make test-esp32    # ESP32 component tests
make test-daisy    # Daisy component tests
make test-clean    # Clean test build artifacts
```

The ESP32 side builds with ESP-IDF's `idf.py` (component-based); the Daisy side is CMake with the libDaisy toolchain file, app placed in QSPI flash via the Daisy bootloader (`BOOT_QSPI`). Testing details: [`docs/testing_guide.md`](docs/testing_guide.md).

## CI and Code Quality

- **GitHub Actions** on pushes/PRs to `main`/`develop`: unit tests (shared protocol, ESP32, Daisy) + build verification for both firmwares, with artifact caching.
- **Pre-commit hooks** handle formatting locally: clang-format (Google style, 4-space indent, 100 col), black + isort, prettier for YAML, plus whitespace/EOF/merge-conflict/large-file checks.

```bash
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
| Link | SPI slave + ATTN out | SPI master |

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
