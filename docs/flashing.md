# Flashing WaveX Firmware

Use this guide to build and install the current WaveX firmware on the ESP32-P4 frontend and Daisy Seed backend. Run the commands from the repository root inside the development container; its toolchains and USB permissions are the supported environment.

## Table of Contents

- [Prerequisites](#prerequisites)
- [ESP32-P4 frontend](#esp32-p4-frontend)
- [Daisy Seed backend](#daisy-seed-backend)
- [Troubleshooting](#troubleshooting)
- [Related](#related)

## Prerequisites

- Build and run from the WaveX devcontainer. For a one-off container command, see the devcontainer instructions in the project `AGENTS.md`.
- Connect the target board by USB and confirm it is passed through to the container. `/dev/ttyACM` numbering is **not stable** — the Daisy re-enumerates on every reset/DFU cycle and can claim `ACM0`, pushing the ESP32 to `ACM1` or beyond — so the Makefile resolves the ESP32 port by USB VID:PID (CH343 bridge, `1a86:55d3`) via `scripts/serial_ports.py esp32` rather than assuming a fixed device name. Only pass `ESP32_PORT=/dev/ttyACMn` if you need to override that resolution. To see what's connected:

  ```bash
  ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
  ```

- Initialize the submodules before the first build:

  ```bash
  make setup
  ```

## ESP32-P4 frontend

Build and flash the frontend with the top-level target:

```bash
make esp32-flash
```

This resolves the port automatically by USB VID:PID at 2,000,000 baud (see Prerequisites). To force a specific port instead, pass `make esp32-flash ESP32_PORT=/dev/ttyACMn`, or run the equivalent ESP-IDF command from `firmware/esp32` directly:

```bash
source /opt/esp/idf/export.sh
idf.py -p /dev/ttyACM1 -b 2000000 flash
```

To flash and open the serial monitor in one command, use:

```bash
make esp32-flash-monitor
```

Exit the ESP-IDF monitor with `Ctrl-]`.

## Daisy Seed backend

The Daisy firmware is a QSPI image loaded through the Daisy bootloader's USB DFU mode. Build it first so that entering DFU mode is the only time-sensitive step:

```bash
make daisy
```

Then start the project flash target:

```bash
make daisy-flash
```

When the target prints its DFU prompt, hold **BOOT** on the Daisy while powering it on or resetting it, then release **BOOT** once it enumerates as a DFU device. The target writes `firmware/daisy/build/wavex-daisy.bin` to the Daisy QSPI address used by this project and tells the bootloader to leave DFU mode.

If the automatic target misses the bootloader window, prepare the binary first and run the DFU transfer manually after placing the board in DFU mode:

```bash
cd firmware/daisy
make bin
dfu-util -d 0483:df11 -s 0x90040000:leave -D build/wavex-daisy.bin
```

> **Note:** The Daisy target is configured for QSPI boot (`BOOT_QSPI`). Do not substitute a generic internal-flash command or an old `dfu-util -a 0` example.

### Touchless flashing (no BOOT/RESET presses)

If the currently running Daisy firmware is still responsive over USB CDC, `make daisy-flash-auto` triggers DFU mode itself (`scripts/daisy_dfu_trigger.py`) instead of requiring the manual BOOT/RESET sequence above. Falls back to the manual sequence being needed only when the board isn't already running WaveX firmware (e.g. first flash, or after a crash).

## Troubleshooting

If the ESP32 command cannot find the board, reconnect it and confirm it shows up with `ls /dev/ttyACM* /dev/ttyUSB*`; force the port with `make esp32-flash ESP32_PORT=/dev/ttyACMn` if VID:PID auto-detection picks the wrong device. If the port is visible on the host but not inside the container, reopen the devcontainer so its USB device mapping is refreshed.

If `dfu-util` cannot find the Daisy, repeat the BOOT-plus-power/reset sequence and run the manual DFU transfer as soon as the device enters DFU mode. The devcontainer includes `dfu-util`; an absent command means the build is not running in the supported container.

## Related

- [Project README](../README.md) for the development quickstart and build commands.
- [Documentation index](README.md) for other working guides.
- [`Makefile`](../Makefile) and [`firmware/daisy/Makefile`](../firmware/daisy/Makefile) for the executable flash targets.
