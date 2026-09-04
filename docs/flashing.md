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

> **Note:** The persistent Daisy target is configured with
> `DAISY_STORAGE=qspi` for QSPI boot. Do not substitute a generic
> internal-flash command or an old `dfu-util -a 0` example.

### Touchless flashing (no BOOT/RESET presses)

If the currently running Daisy firmware is still responsive over USB CDC, `make daisy-flash-auto` triggers DFU mode itself (`scripts/daisy_dfu_trigger.py`) instead of requiring the manual BOOT/RESET sequence above. Falls back to the manual sequence being needed only when the board isn't already running WaveX firmware (e.g. first flash, or after a crash).

## Debug-probe workflows (SWD, not DFU)

DFU/QSPI is the persistent path and needs no extra hardware. For the ordinary
Daisy edit/test loop, an ST-Link can load a separate SRAM-linked ELF without a
QSPI erase, DFU enumeration, boot-mode switch, or post-load reset:

| Task | Command |
|---|---|
| Build + SRAM load + run | `make daisy-debug` |
| Build SRAM ELF only | `make daisy-debug-build` |
| GDB server | `make daisy-debug-server` — listens on `localhost:3333` |
| Load through existing server | In a second shell, `make daisy-debug-load` |
| VS Code | Cortex-Debug launch config: `"servertype": "openocd"`, `"gdbTarget": "localhost:3333"` |

`make daisy-debug` uses `firmware/daisy/build-debug/`, starts a temporary
OpenOCD server, and lets GDB load the ELF's addressed sections into SRAM before
resuming `Reset_Handler`. It resets before loading and deliberately does not
reset afterward. The persistent QSPI application at `0x90040000` is untouched;
resetting or power-cycling the board boots that image again. Rebuild the
devcontainer image after pulling this workflow so `gdb-multiarch` is present.

A 2026-09-04 hardware run with both outputs already built measured the complete
load-and-launch commands at 19.991 s for software-triggered DFU/QSPI and 2.870 s
for SWD/SRAM. That single-board result makes the volatile path about 7x faster;
repeat it when host USB or probe hardware changes rather than treating it as a
fixed specification.

The two Daisy profiles intentionally have different memory timing:

| Content | Persistent QSPI profile | Fast SRAM profile |
|---|---|---|
| Executable code | Memory-mapped QSPI | D1 AXI SRAM |
| Initialized globals | D1 AXI SRAM | DTCM |
| Ordinary non-DMA state | D1 AXI SRAM | D2 SRAM, with parser-only spill in D3 |
| Explicit hot callback state | DTCM | DTCM |
| SAI/UART DMA buffers | Dedicated D2 DMA region | Same dedicated D2 DMA region |
| SDMMC1/FatFS I/O buffers | D1 AXI SRAM | D1 AXI SRAM (SDMMC1 cannot reach D2/D3) |

`DEBUG_OPT` defaults to `-O0`, matching the current default persistent build,
so the profile does not add an optimization-level difference on top of these
memory-placement differences.

That makes the SRAM profile suitable for functional work, debugger use, and
controlled comparisons made with matching compiler flags and workloads. It is
not a substitute for profiling the release memory layout: make production DWT
headroom and zero-underrun claims on the persistent QSPI profile.

The ESP32-P4 can also flash over its native USB DFU (`idf.py dfu-flash`) if
the UART pins are otherwise occupied; `make esp32-flash` does not need this.

## Troubleshooting

If the ESP32 command cannot find the board, reconnect it and confirm it shows up with `ls /dev/ttyACM* /dev/ttyUSB*`; force the port with `make esp32-flash ESP32_PORT=/dev/ttyACMn` if VID:PID auto-detection picks the wrong device. If the port is visible on the host but not inside the container, reopen the devcontainer so its USB device mapping is refreshed.

If `dfu-util` cannot find the Daisy, repeat the BOOT-plus-power/reset sequence and run the manual DFU transfer as soon as the device enters DFU mode. The devcontainer includes `dfu-util`; an absent command means the build is not running in the supported container.

## Related

- [Project README](../README.md) for the development quickstart and build commands.
- [Documentation index](README.md) for other working guides.
- [`Makefile`](../Makefile) and [`firmware/daisy/Makefile`](../firmware/daisy/Makefile) for the executable flash targets.
