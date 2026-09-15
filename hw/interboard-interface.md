# WaveX Inter-board Electrical Interface Contract

**Status:** signal contract for schematic capture. Connector families/pin numbers are not frozen until enclosure geometry is known.

The purpose of this file is to prevent board designs from inventing interfaces independently. Every signal crossing a PCB boundary must appear here.

## 1. Board set

- **UI** — ESP32-P4-Core-DEV-KIT + panel hardware.
- **BACKEND** — VisionSOM-RT1176 carrier.
- **ANALOG** — AD1938 + CV DAC + eight filters + stereo gain/pan/mix.
- **JACK** — optional passive/mechanical board only if enclosure layout requires it.

## 2. Electrical domains

| Domain | Nominal | Notes |
|---|---:|---|
| Logic | 3.3 V | ESP32/RT1176 peripheral logic and digital controls unless explicitly noted |
| System/UI | 5 V | display and compute-board input distribution |
| Analog + | +12 V target | SSI/op-amp rail; final power design TBD |
| Analog - | -12 V target | SSI/op-amp rail; final power design TBD |
| Chassis/shield | TBD | enclosure/jack/USB shield policy must be defined before fabrication |

Do not route 1.75 V SNVS-domain RT1176 control signals off the backend board unless the specific circuit is designed for that domain.

## 3. UI ↔ BACKEND interface

### 3.1 Functional requirement

The frontend owns UI, touch, panel controls and MIDI. The backend owns time-critical sequencing/audio/storage. The link transports commands, state/readback and telemetry; sample audio never crosses this interface.

Rev A migration uses UART as the guaranteed transport and wires SPI in parallel for later qualification.

### 3.2 Signal set

| Net | Source | Destination | Level | Requirement |
|---|---|---|---:|---|
| UI_UART_TX | UI | BACKEND LPUART1 RX | 3.3 V | live migration/control link |
| UI_UART_RX | BACKEND | UI | 3.3 V | live migration/control link |
| UI_SPI_SCLK | BACKEND | UI | 3.3 V | reserved high-speed link, backend master |
| UI_SPI_MOSI | BACKEND | UI | 3.3 V | backend → frontend |
| UI_SPI_MISO | UI | BACKEND | 3.3 V | frontend → backend |
| UI_SPI_CS_N | BACKEND | UI | 3.3 V | active low |
| UI_ATTN | UI | BACKEND | 3.3 V | pending frontend data / attention |
| UI_RESET_REQ | BACKEND | UI | 3.3 V | optional, reserve if connector budget allows; exact reset behavior TBD |
| +5V_UI | power distribution | UI | 5 V | optional on same connector; current rating must cover UI design |
| GND | — | — | 0 V | multiple pins, interleave with fast signals |

### 3.3 Recommended connector budget

If signal and UI power share one harness, reserve **at least 16 positions** so grounds can be interspersed:

```text
GND   UART_TX
GND   UART_RX
GND   SPI_SCLK
GND   SPI_MOSI
GND   SPI_MISO
GND   SPI_CS_N
GND   UI_ATTN
GND   +5V_UI
```

This is a topology recommendation, not a final physical pin numbering.

If the display/UI current makes a ribbon/mezzanine power path unattractive, use a separate two-wire/high-current 5 V connector and keep the signal connector logic-only.

### 3.4 Link layout

- Treat SPI as a fast digital interface even if initial firmware uses UART.
- Ground-adjacent conductors are strongly preferred for SCLK and data lines across a cable.
- Provide source-series resistor footprints on SCLK and backend MOSI.
- Keep connector/cable length as short as the enclosure permits.
- UART remains the fallback if SPI integrity at the required rate is poor.
- No level shifter is expected for a 3.3 V-to-3.3 V implementation.

## 4. BACKEND ↔ ANALOG digital interface

### 4.1 SAI1

| Net | Source | Destination | Level |
|---|---|---|---:|
| AUD_MCLK | RT1176 | AD1938 | 3.3 V logic |
| AUD_BCLK | RT1176 | AD1938 | 3.3 V logic |
| AUD_FSYNC | RT1176 | AD1938 | 3.3 V logic |
| AUD_TX | RT1176 | AD1938 DAC serial input | 3.3 V logic |
| AUD_RX | AD1938 ADC serial output | RT1176 | 3.3 V logic |

Target is 48 kHz, 8 × 32-bit slots, BCLK 12.288 MHz.

Provide ground conductors adjacent to clock lines. This interface should use a board-to-board/mezzanine connection rather than a long loose harness.

### 4.2 Shared control SPI

| Net | Source | Destination |
|---|---|---|
| CTRL_SCK | RT1176 | 4 × MCP48CVB28 + AD1938 |
| CTRL_MOSI | RT1176 | DAC SDI + AD1938 CIN |
| CTRL_MISO | DAC/codec selected device | RT1176 |
| DAC0_CS_N | RT1176 | MCP48CVB28 #0 |
| DAC1_CS_N | RT1176 | MCP48CVB28 #1 |
| DAC2_CS_N | RT1176 | MCP48CVB28 #2 |
| DAC3_CS_N | RT1176 | MCP48CVB28 #3 |
| CODEC_CS_N | RT1176 | AD1938 CLATCH/CS function as required by final SPI wiring |
| DAC_LAT_EVEN | RT1176 | all DAC LAT0 |
| DAC_LAT_ODD | RT1176 | all DAC LAT1 |
| CODEC_RESET_N | RT1176 | AD1938 PD/RST |

Confirm the final AD1938 SPI signal naming/polarity in schematic capture; the interface above expresses ownership, not the codec pin label.

### 4.3 Suggested digital connector size

Signal count is 5 SAI + 3 shared SPI + 4 DAC CS + 2 LAT + codec CS + codec reset = **16 signals** before spare/debug lines.

Use at least a **2 × 12 / 24-position** connector or equivalent and dedicate the extra positions to ground and spare GPIO. Suggested grouping:

```text
GND / MCLK
GND / BCLK
GND / FSYNC
GND / AUD_TX
GND / AUD_RX
GND / CTRL_SCK
GND / CTRL_MOSI
GND / CTRL_MISO
DAC0_CS / DAC1_CS
DAC2_CS / DAC3_CS
LAT_EVEN / LAT_ODD
CODEC_CS / CODEC_RESET
```

A higher-density shielded/mezzanine connector is preferred if board stacking allows it.

## 5. Power to ANALOG board

Do not source the analog board from the VisionSOM 3.3 V auxiliary output.

The analog board needs an independent power interface capable of providing:

- +12 V analog,
- -12 V analog,
- a suitable positive input from which clean codec/DAC 3.3 V rails can be generated, or pre-regulated clean 3.3 V rails if the system power design chooses that topology,
- ground.

Keep high-current/power conductors separate from the SAI/control connector unless a selected mezzanine system is explicitly rated and laid out for both.

Final pin count, current rating, connector and protection depend on the system power-supply design.

## 6. UI external connectors

Frontend-owned external connections:

- USB MIDI/device,
- DIN MIDI IN,
- DIN MIDI OUT.

If a passive rear JACK board is used for these connectors, the JACK board carries only connector/protection/buffer circuitry; logical ownership remains UI.

Do not run MIPI-DSI to the rear jack board.

## 7. ANALOG external connectors

Analog-board-owned external connections:

- sampling/audio input Left/Right,
- Main Out Left/Right,
- headphone output if implemented.

Any future CV/Gate or individual voice outputs belong to the analog/backend side and must be added here before schematic work claims connector pins.

## 8. Optional JACK board

Prefer placing user jacks directly on the owning board when mechanics allow it. A passive jack board is justified only to satisfy enclosure geometry.

If required:

- use separate harness groups for sensitive analog audio and digital MIDI/USB,
- do not bundle high-speed DSI/SAI clocks with analog inputs,
- provide shield/chassis strategy at the jack board rather than allowing random chassis contacts through mounting hardware.

## 9. Grounding / chassis rules

Current design principle:

- boards share a solid electrical ground through low-impedance interconnects,
- high-speed return paths must travel adjacent to their signals,
- analog and digital current paths are controlled with placement/routing rather than isolated ground islands,
- USB shield and metal chassis connection require an explicit RC/direct/ESD policy after enclosure material is selected.

Do not introduce a `DGND`/`AGND` split merely because components are labeled analog/digital. Any split must be justified by return-current analysis.

## 10. Bring-up order

1. Power each board independently with current limiting.
2. UI: Core + display/touch only.
3. UI: keypad, ADC/controls and LEDs.
4. Backend: SOM boot/debug/SD/USB.
5. UI ↔ backend UART.
6. Backend ↔ analog: SAI clocks with codec held reset.
7. AD1938 register control and TDM loopback.
8. One CV DAC and one analog filter/mixer lane.
9. Eight-lane expansion.
10. Qualify frontend SPI; only then consider replacing UART as the guaranteed control link.

## 11. Open connector decisions

Before PCB placement is frozen:

- board stacking versus cable-separated layout,
- actual maximum UI↔backend cable length,
- actual backend↔analog spacing,
- connector family/current rating,
- keying/polarization,
- service/disassembly cycles,
- chassis/shield scheme,
- whether +5 V UI power rides with logic or on a separate connector.
