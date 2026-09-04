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
- Connect the target board by USB and confirm it is passed through to the container. `/dev/ttyACM` numbering is **not stable** — the Daisy re-enumerates on every reset/DFU cycle and can claim `ACM0`, pushing the ESP32 to `ACM1` or beyond — so the Makefile resolves ESP32 ports by USB VID:PID via `scripts/serial_ports.py` rather than assuming a fixed device name. Two ports reach the P4: the board's CH343 UART bridge (`1a86:55d3`, board name `esp32`) carries the firmware console, and the P4's own USB connector enumerates as the chip's built-in USB-Serial/JTAG unit (`303a:1001`, board name `esp32-jtag`). Only pass `ESP32_PORT=/dev/ttyACMn` if you need to override that resolution. To see what's connected:

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

This resolves the port automatically by USB VID:PID at 2,000,000 baud (see Prerequisites). When the P4's own USB connector is plugged in, the target flashes through the built-in USB-Serial/JTAG unit: the ROM bootloader serves it with no firmware involvement, esptool enters download mode and resets back over it, and the console on the CH343 bridge is never touched. The 2,000,000 baud setting is harmless there (USB ignores it) and applies when only the bridge is connected, which is the fallback. The USB MIDI device runs on the P4's separate high-speed OTG controller, so it does not displace the USB-Serial/JTAG unit while the app runs; a working `esptool.py --port <jtag port> chip_id` round trip with the app running was confirmed on 2026-09-04. To force a specific port instead, pass `make esp32-flash ESP32_PORT=/dev/ttyACMn`, or run the equivalent ESP-IDF command from `firmware/esp32` directly:

```bash
source /opt/esp/idf/export.sh
idf.py -p "$(python3 ../../scripts/serial_ports.py esp32-jtag)" -b 2000000 flash
```

To flash and open the serial monitor in one command, use:

```bash
make esp32-flash-monitor
```

This flashes over the preferred port and then monitors the CH343 bridge, because the console is on UART0. Monitoring the USB-Serial/JTAG port shows nothing. Exit the ESP-IDF monitor with `Ctrl-]`.

## Both boards in one go

```bash
make flash-fast     # ESP32 persistent over USB-JTAG + Daisy into SRAM over SWD, concurrently
make flash-all      # ESP32 + Daisy persistent (software-triggered DFU, ~20 s); stops/restarts the loggers
```

`flash-fast` is the edit/test loop: neither path touches a console port, so
`make logs-start` loggers stay attached and simply see each board reboot, and
the two paths share no USB device so they run in parallel. Measured from the
devcontainer on 2026-09-04 with both images already built: 10.8 s wall for
both boards (ESP32 10.6 s, Daisy 3.4 s). The Daisy side is the volatile SRAM
image, so a reset or power cycle returns it to whatever QSPI holds - use
`flash-all` (or `make daisy-flash-auto`) when the Daisy change has to persist.
The target fails, per board, if either flash did.

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
| Load through existing server | In a second shell, `make daisy-debug-load` — fails fast if nothing is listening on the GDB port, and fails (non-zero, transcript in `build-debug/gdb-load.log`) if GDB reports the load did not complete |
| VS Code | Cortex-Debug launch config: `"servertype": "openocd"`, `"gdbTarget": "localhost:3333"` |

`make daisy-debug` uses `firmware/daisy/build-debug/`, starts a temporary
OpenOCD server, and lets GDB load the ELF's addressed sections into SRAM before
resuming `Reset_Handler`. It resets before loading and deliberately does not
reset afterward. The persistent QSPI application at `0x90040000` is untouched;
resetting or power-cycling the board boots that image again. Rebuild the
devcontainer image after pulling this workflow so `gdb-multiarch` is present.

All three SWD targets first confirm an ST-Link is enumerated on USB, resolved
by VID:PID through `scripts/serial_ports.py --present stlink` the same way the
DFU targets look for the Daisy. The Daisy's own CDC port is not required: a
blank or crashed board is still loadable over SWD. If the probe is missing the
target stops before building anything and says so, rather than letting OpenOCD
fail on an absent adapter. `make daisy-debug-load` additionally refuses to run
when nothing is listening on the GDB port, so a forgotten `daisy-debug-server`
produces one line instead of a GDB timeout followed by a page of errors.

OpenOCD occasionally starts with `target stm32h7x.cpu0 examination failed`
(seen once in five back-to-back runs on 2026-09-04, right after the previous
server was killed mid-session). The probe is fine but the Daisy did not answer
on SWD, and every GDB connection then fails with `Target not examined yet`.
`make daisy-debug` detects this and aborts; with `daisy-debug-server`, watch
for the warning and restart the server.

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

`DEBUG_OPT` defaults to `-O0` for stepping; the persistent build is `-O2`
(`WAVEX_DAISY_OPT`, since 2026-09-04), so the debug profile differs from it in
optimization level as well as in memory placement. Pass `DEBUG_OPT=-O2` to
remove that difference when chasing something timing-sensitive.

That makes the SRAM profile suitable for functional work, debugger use, and
controlled comparisons made with matching compiler flags and workloads. It is
not a substitute for profiling the release memory layout: make production DWT
headroom and zero-underrun claims on the persistent QSPI profile.

The ESP32-P4's own USB connector is the normal flash path (see the frontend
section above). Its ROM also offers USB DFU (`idf.py dfu-flash`) on the same
connector; `make esp32-flash` does not need this.

## Troubleshooting

**ESP32 stuck in download mode** (`rst:0x17 ... boot:0x307 (DOWNLOAD...)` and
`waiting for download` on the console after every reset, app never starts,
Daisy reports `rx 0 B`): an esptool session over the CH343 bridge that aborted
mid-way - typically because a logger was reading the same port (`device
reports readiness to read but returned no data`) - left the bridge's DTR/RTS
holding the BOOT strap. `make esp32-reset` runs one complete esptool session
over the bridge (stopping and restarting the loggers around it), which
releases the lines and boots the app; measured 3 s. Note that the ROM serves
the USB-JTAG port in download mode too, so `serial_ports.py esp32-jtag` being
present does not prove the app is running - the console or the boot mode line
does.

If the ESP32 command cannot find the board, reconnect it and confirm it shows up with `ls /dev/ttyACM* /dev/ttyUSB*`, or ask the resolver directly with `python3 scripts/serial_ports.py esp32-jtag` and `... esp32`; force the port with `make esp32-flash ESP32_PORT=/dev/ttyACMn` if VID:PID auto-detection picks the wrong device. If the port is visible on the host but not inside the container, reopen the devcontainer so its USB device mapping is refreshed. The USB-Serial/JTAG node is owned by group `plugdev` rather than `dialout` on the host; the devcontainer user is in both.

If `dfu-util` cannot find the Daisy, repeat the BOOT-plus-power/reset sequence and run the manual DFU transfer as soon as the device enters DFU mode. The devcontainer includes `dfu-util`; an absent command means the build is not running in the supported container.

## Related

- [Project README](../README.md) for the development quickstart and build commands.
- [Documentation index](README.md) for other working guides.
- [`Makefile`](../Makefile) and [`firmware/daisy/Makefile`](../firmware/daisy/Makefile) for the executable flash targets.
