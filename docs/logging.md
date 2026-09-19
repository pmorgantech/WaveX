# Debug logging: per-module levels, tuned at runtime

The problem this solves: deep-diving one subsystem used to mean either
rebuilding with a `#define` flipped or drowning in every other subsystem's
chatter. Logging is now gated per module and per level, and the runtime
levels are adjustable over each board's console — no reflash.

## The model (both MCUs)

Defined in [`firmware/shared/config/logging_config.h`](../firmware/shared/config/logging_config.h).
Call sites use `WAVEX_LOGE/W/I/D/T(MODULE, fmt, ...)` — ERROR, WARN, INFO,
DEBUG, TRACE. Every call passes two gates:

1. **Compile-time ceiling** — `WAVEX_LOG_CEILING_<MODULE>`, default TRACE
   (everything present in the binary). Lower a module's ceiling only when
   the disabled runtime check itself is too hot for its call sites; the
   call compiles away entirely above the ceiling.
2. **Runtime level** — a per-module byte, default from the module table.
   This is what the console commands change.

The module list is one X-macro table (`WAVEX_LOG_MODULE_LIST`) — adding a
module there is the whole job. Current modules: SYSTEM, INTER_MCU_LINK,
UART_PROTOCOL, UART_PERF, SPI_LINK, AUDIO_ENGINE, STORAGE, SD, STREAM, CV,
SEQUENCER, MIDI.

Emission is per-platform: ESP32 → `ESP_LOG` (tag `WAVEX-<MODULE>`) over native USB Serial/JTAG, Daisy →
the non-blocking log ring (`comm/log_ring.h`; its main-loop-only rule applies
to log calls unchanged), host tests → `printf`.

## Tuning at runtime

The command grammar is identical on both boards (parsed by the shared
`ApplyLevelCommand`, host-tested):

```
WAVEX-LOG <MODULE|*> <OFF|ERROR|WARN|INFO|DEBUG|TRACE|0-5>
WAVEX-LOG ?              # list every module's current level
```

- **Daisy**: send the line to the CDC port (same port as the DFU trigger);
  the USB ISR captures it, the main loop applies it and confirms through
  the log ring.
- **ESP32**: send it to the native USB console (same listener as the screenshot
  token). Names that match no module are applied as verbatim IDF tags via
  `esp_log_level_set` — that is how the 400+ plain `ESP_LOGx` call sites
  (tags like `UI_NAVIGATOR`, `packet_router`) are tuned. Module names are
  mirrored into their `WAVEX-<MODULE>` tag automatically, because on the
  ESP32 both the module byte *and* IDF's per-tag level gate the output.

The comfortable way is the configurator:

```
scripts/wavex_log.py --list                      # module table
scripts/wavex_log.py daisy INTER_MCU_LINK DEBUG  # deep-dive one subsystem
scripts/wavex_log.py daisy '*' WARN              # quiet the Daisy
scripts/wavex_log.py esp32 UI_NAVIGATOR DEBUG    # per-IDF-tag
scripts/wavex_log.py esp32 '?'                   # current module levels
```

The per-voice filter (topology, mode, slope and drive) is entirely
Instrument-owned and edited on the Instrument page's Filter tab; there is no
console switch for it. The `WAVEX-DBG <seq> EDIT <track>` readback reports
the current `mode`, `topology`, `slope` and `drive` (thousandths).

It is a listening aid, not a parameter: nothing on the wire or in the UI
sets it, and it does not survive a reboot.

It writes to the port without claiming it (coexists with `serial_log.py`)
and tails `logs/<board>.log` for the confirmation line.

The same line reader carries the acknowledged test grammar, `WAVEX-DBG <seq>
<VERB> ...` → `WAVEX-DBG: <seq> OK|ERR ...`, which `tests/hil/` drives
(input injection, synthetic touch, `STATE` queries, Daisy message
injection): see [`features/debug-harness-and-hil.md`](features/debug-harness-and-hil.md)
§10 for the verbs. `WAVEX-LOG` is also accepted as a `WAVEX-DBG` verb.

Runtime changes do not persist across reboot; boot-time defaults are the
module table (and `sdkconfig`'s `CONFIG_LOG_DEFAULT_LEVEL` for plain ESP32
tags). Change the table when a different default has earned its place.

## ESP32 specifics

The console uses the P4's built-in USB Serial/JTAG controller
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`), independently of TinyUSB MIDI on the
high-speed OTG controller. `serial_ports.py esp32` and `esp32-jtag` resolve
that port; `esp32-uart` resolves the CH343 bridge for ROM recovery. Normal logs,
commands, HIL replies and screenshots all use native USB. Its tty baud setting
does not set the transfer rate. The UART build alternative remains supported
by the console reader, but requires explicit host port overrides.

Debug builds install ESP-IDF's interrupt-driven USB driver with a 4 KiB TX
queue and 1 KiB RX queue and route stdout through its VFS. Commands keep their
existing dedicated task and UI mailbox. Before that initialization (and in
release builds), ESP-IDF's default USB console implementation handles output.
Use the Make flash/monitor targets: they pause the managed ESP32 logger and
resume it, without rotating its file, after success or failure. Custom loggers
must be stopped separately; only one reader may own a console at a time.

This is the serial endpoint of USB Serial/JTAG, not binary JTAG apptrace.
The latter's IDF 5.5 formatter does not support dynamic string arguments or
64-bit/double arguments, so it is not a drop-in replacement for these logs.
See [Espressif's console guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-guides/usb-serial-jtag-console.html)
and [apptrace limitations](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-guides/app_trace.html#limitations).

`sdkconfig` sets `CONFIG_LOG_MAXIMUM_LEVEL=VERBOSE` with
`CONFIG_LOG_DEFAULT_LEVEL=INFO`: DEBUG/VERBOSE call sites exist in the
binary but are silent until enabled per tag. (Previously
`MAXIMUM_EQUALS_DEFAULT` compiled them out — you could silence tags at
runtime but never turn detail on.)

## Choosing a level at a call site

- **ERROR** — something failed; visible at every useful setting.
- **WARN** — surprising but survivable.
- **INFO** — lifecycle landmarks (boot, init, mode changes). The default
  level, so keep it to lines a person wants during normal bring-up.
- **DEBUG** — per-operation detail (a browse request, a parameter apply).
- **TRACE** — per-packet / per-event firehose.

Legacy `WAVEX_LOG_DAISY(MODULE, ...)` calls are INFO via an alias; when
touching one, give it a real level. The `UART_LOGx` macros
(`uart_debug_config.h`) are still their own compile-time system — folding
them into the module table is tracked in `docs/roadmap.md`.

## Optional Daisy RTT experiment

`WAVEX_RTT_LOGGING=ON` adds an independent, nonblocking mirror of foreground
log writes. USB logs and console input remain active. This is a bench option,
off in normal builds; it does not authorize logging from the audio callback or
an ISR. The 8 KiB RTT ring and control block occupy the existing uncached D2
region, with a Cortex-M7 memory barrier before publishing written bytes.
`WAVEX-DBG <seq> LOGSTATS` reports `usb_drop`, `isr_log`, and, in RTT builds,
`rtt_drop`. An absent reader can fill the RTT ring and increase its loss count
without blocking the main loop. Loss counters describe bytes, not whole lines.

The optional dependency is the official SEGGER implementation, rather than a
second custom RTT protocol. Normal builds neither download nor link it. Fetch
the tested revision inside the devcontainer:

```bash
git clone https://github.com/SEGGERMicro/RTT.git /tmp/wavex-segger-rtt
git -C /tmp/wavex-segger-rtt checkout 4d8feab3150f86f37a9d323ddc88d6cdf5673072
make -C firmware/daisy BUILD_DIR=build-rtt-qspi \
  CMAKE_EXTRA_ARGS="-DWAVEX_RTT_LOGGING=ON -DWAVEX_SEGGER_RTT_DIR=/tmp/wavex-segger-rtt -DWAVEX_PROFILING_ENABLED=ON" \
  bin -j$(nproc)
```

Preserve the normal QSPI binary before flashing the experiment. Follow the
DFU procedure in [flashing.md](flashing.md); restore the saved binary afterward.
The full SRAM debug image currently exceeds its independent D2/DTCM limits;
this experiment does not relax those limits or change the SRAM linker layout.

Resolve `_SEGGER_RTT` from the **tested ELF** with `arm-none-eabi-nm`, then
configure OpenOCD `rtt setup <address> <control-block-size> "SEGGER RTT"`,
`rtt start`, and `rtt server start 9090 0`. Never reuse another image's address.
The debugger must read while the CPU runs; do not use halt/semihosting logging.
With the normal USB logger active, run:

```bash
python3 scripts/bench_daisy_rtt.py --usb-log logs/daisy.log --bursts 100
```

The script drains old RTT backlog, requests one module-level listing at a time
(the Daisy console accepts only one pending command), compares every output
byte and checks loss/underrun counters. It reports complete burst delivery
latency, not theoretical bus throughput. Hardware results and remaining gates
are recorded at [HV-021](hardware-validation.md#hv-021--usb-console-and-daisy-rtt).

**2026-09-19 result:** RTT over the current ST-Link V2/OpenOCD setup failed
byte-for-byte integrity checks, even at reduced SWD speed. It remains an
experiment; keep the USB log ring and normal USB transport. The no-reader
check passed with idle audio callbacks, but active-audio soak testing remains
open. See [HV-021](hardware-validation.md#hv-021--usb-console-and-daisy-rtt).
