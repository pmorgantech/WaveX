# ESP32-P4-Core carrier and analog panel controls

**Status:** Target hardware decision, 2026-09-14. A selectable Core pin profile
exists; Core boot, display, communications and panel operation remain unverified.
This note records the requested board/control change for Phase 2.P and corrects
the initial allocation against the vendor schematic and current WaveX wiring.

## Contents

- [Decision](#decision)
- [Corrections to the initial proposal](#corrections-to-the-initial-proposal)
- [Control and data model](#control-and-data-model)
- [Additional parts](#additional-parts)
- [Build and bring-up boundary](#build-and-bring-up-boundary)
- [Related and sources](#related-and-sources)

## Decision

Target the **Waveshare ESP32-P4-Core-DEV-KIT** carrier for the physical panel.
This is not a pinout for the bare ESP32-P4-Core module. Preserve the WIFI6 bench
profile while Core hardware is brought up. The Core offers bottom-pad expansion
and dedicated display/camera connectors; it does not make every exposed net
freely assignable. The vendor lists 32 MB NOR flash and 32 MB packaged PSRAM.
The existing image/partition layout uses the lower 16 MB; expanding it is separate
work after memory and boot verification. [Board reference][board]

Prefer four **Taiwan Alpha RV112FF-40B1**, dual 10 kΩ linear, continuous-rotation
potentiometers for the smooth parameter controls. The proposed order string is
`RV112FF-40B1-15F-0B10K-0068`: retain it as a purchasing candidate, with the
supplier confirming the final suffix and current drawing before PCB fabrication.
The family datasheet confirms dual units, 360° travel, linear resistance options,
and the B1 metal shaft/threaded bushing. The B3N alternative has a metal bushing
without thread. Prefer B1 so the front panel carries mechanical load.

Alpha specifies **15,000 rotation cycles**; evaluate that rating against expected
use and replacement access. Retain two detented PCNT navigation encoders for
frequent selection/data entry. The datasheet does not establish the actual
wiper phase offset or transfer curve: scope samples before choosing a decoder.
[Alpha datasheet][alpha]

## Corrections to the initial proposal

The authoritative assignments and reservations are in
[`pin_config.h`](../../firmware/shared/config/pin_config.h); board selection and
analog channel mapping are in
[`hardware_config.h`](../../firmware/shared/config/hardware_config.h).

The [Core schematic][schematic] confirms the main-header inventory, but corrects
the first claimed bottom-pad entry. It also shows the display I²C connection,
onboard RGB LED, boot circuit and USB/UART bridge. These nets must be accounted
for before allocating controls. Keep the display on the BSP-owned I²C bus;
its current GT911 path polls touch and has no separate interrupt/reset GPIO.
MIPI differential lanes remain dedicated PHY nets. The current repository had
already removed the obsolete GPIO-based DSI definitions before this proposal.

The revised Core profile preserves the live Daisy UART, DIN MIDI reservation,
both PCNT pairs and USB reservations. Low-speed keypad IRQ, LED reset/output-enable,
ADC chip selects and mux addresses use bottom pads, leaving **five main-header
GPIOs free**, in addition to the reserved future USB-host pair. The exact free
pool and pad locations are recorded only in the pin header. A carrier or soldered
breakout and continuity checks are required to use bottom pads.

Use two **NXP PCA9956BTWY** LED drivers on the BSP-owned I²C bus, sharing
it with touch and keypad. SPI2 serves only the panel ADCs; SPI3 remains reserved
for the Daisy link. The replacement changes the LED interface and brightness
resolution; the existing 48-channel logical map still fits two devices.
See [panel-controls.md §3.3](panel-controls.md#33-pca9956btwy-led-interface)
for initialization, ownership and electrical constraints.

## Control and data model

| Entity | Relationship and ownership |
|---|---|
| Navigation encoder | Two digital A/B signals to one PCNT unit; two units total. Push switches belong to the keypad matrix. |
| Panel switch | One TCA8418 keycode maps to one logical key, including encoder pushes and pads. |
| RV112FF control | Two analog wipers form one calibrated angle/delta source; four controls occupy all eight inputs of the first MCP3208. |
| Conventional pot | A separate MCP3208 accepts a 74HC4067 mux; mux channel and ADC channel identify each reading. Remaining ADC inputs are available for direct inputs. |
| LED | Logical LED maps to one flattened PCA9956B channel; the UI publishes a complete frame to the panel task. |

The dedicated eight-input ADC avoids an external mux between each RV112FF pair.
It still converts sequentially: bound pair skew, source impedance and acquisition
settling. The second ADC/mux path must settle after address changes and must not
delay endless-pair sampling. Its population and scan budget remain open.

The MCP3208 replaces the earlier MCP3008 plan. Use its 12-bit transfer framing
and result extraction; changing a resolution constant alone is insufficient.
The initial clock follows the conservative low-voltage limit in the
[Microchip datasheet][adc]; verify acquisition settling with the actual pots.
No ADC, mux or LED driver is enabled by these config reservations. One future
`panel_task` owns ADC SPI2 and the two I²C LED device handles, consumes complete
LED snapshots, and publishes complete input events. LED transfers and retries
are bounded so they do not monopolize touch/keypad I²C or delay ADC sampling. See
[panel-controls.md](panel-controls.md) for the software ownership model.

## Additional parts

Parts recorded for hardware planning on 2026-09-14:

- **MCP48CMB28-20E/ST**
- **PA0035** adapter

## Build and bring-up boundary

Prerequisites: the supported devcontainer, initialized dependencies, the Core
carrier revision matching the schematic, and the existing HX8394/GT911 display.
Use the build/flash procedure in [flashing.md](../flashing.md). Inside the
container, select Core in an independent build directory:

```bash
cd /workspaces/WaveX/firmware/esp32
idf.py -B build-core -DWAVEX_ESP_BOARD=CORE build
```

The default remains WIFI6. Both profiles reject duplicate, unexposed and reserved
GPIO claims at compile time. Reuse the current display BSP only for its matching
DSI/I²C path; its unrelated SD, codec and radio assumptions are not Core support.
Do not enable those peripherals during bring-up. Core has two Type-C functions:
USB/UART bridge and native USB; verify console/flash port selection on the board.

Implementation order and all open acceptance checks live in
[roadmap Phase 2.P](../roadmap.md#2p--panel-controls-and-midi-io-physical-integration)
and [hardware verification](../roadmap.md#outstanding-hardware-verification).
Compilation cannot establish boot, panel rendering, USB enumeration, knob feel,
latency or uninterrupted audio. WIFI6 bench results do not transfer to Core.

## Related and sources

- [Architecture](../architecture.md#32-authoritative-configuration-files)
- [Panel controls](panel-controls.md)
- [Waveshare Core board reference][board]
- [Waveshare Core carrier schematic, sheet 1][schematic]
- [Taiwan Alpha RV112FF family datasheet][alpha]
- [Microchip MCP3204/3208 datasheet, sections 5–6][adc]
- [NXP PCA9956B datasheet](https://www.nxp.com/docs/en/data-sheet/PCA9956B.pdf)

[board]: https://docs.waveshare.com/ESP32-P4-Core-DEV-KIT
[schematic]: https://files.waveshare.com/wiki/ESP32-P4-Core-DEV-KIT/ESP32-P4-Core-DEV-KIT.pdf
[alpha]: https://www.taiwanalpha.com/downloads?target=products&id=79
[adc]: https://ww1.microchip.com/downloads/en/DeviceDoc/21298e.pdf
