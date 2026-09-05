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

Emission is per-platform: ESP32 → `ESP_LOG` (tag `WAVEX-<MODULE>`), Daisy →
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
- **ESP32**: send it to the console UART (same listener as the screenshot
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

The same console carries the per-voice filter A/B switch on the Daisy
(debug builds; `audio/voice_filter.hpp`):

```
WAVEX-FILTER ?                          # current selection
WAVEX-FILTER <wavex|daisysp> [12|24] [drive 0-100]
scripts/wavex_filter.py daisysp         # the DaisySP Svf
scripts/wavex_filter.py wavex 24 60     # first-party SVF, 24 dB, 60% drive
```

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
them into the module table is tracked in `docs/backlog.md`.
