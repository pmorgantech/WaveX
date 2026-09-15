# WaveX Frontend / UI Electrical Specification

**Status:** schematic-ready architecture; fabrication blocked on the verification items in §10.

## 1. Scope

This board is the WaveX human-interface board. It carries the panel controls and connects the Waveshare ESP32-P4-Core-DEV-KIT to the display and to the RT1176 backend.

Production frontend requirements:

- Waveshare **ESP32-P4-Core-DEV-KIT** carrier target; no Wi-Fi or Bluetooth subsystem is required.
- Waveshare **8-DSI-TOUCH-A** display.
- Two detented quadrature navigation encoders.
- Four dual-track continuous RV112FF parameter controls.
- TCA8418 button/pad matrix.
- Two PCA9956BTWY constant-current LED drivers.
- One MCP3208 8-channel ADC for the four dual-track controls.
- DIN MIDI input/output and USB MIDI.
- UART and reserved SPI connection to the RT1176 backend.

There is **no second MCP3208 and no 74HC4067** in the current product plan.

## 2. Display

Target: Waveshare **8-DSI-TOUCH-A**.

Vendor-confirmed characteristics:

- 8-inch IPS panel.
- Native portrait resolution: **800 × 1280**.
- WaveX orientation: **1280 × 800 landscape**.
- MIPI-DSI, **2 data lanes** when used with ESP32-P4.
- Up to 60 Hz refresh.
- Display controller: **JD9365**.
- Touch controller: **GT9271**, I2C, up to 10 touch points.
- Optical-bonded toughened glass.
- Nominal display supply: **5.0 V**, vendor electrical table gives 4.75–5.30 V and 0.8 A typical input current.
- Backlight control is performed over the display-side I2C controller; Waveshare documents brightness 0x00–0xFF through I2C device 0x45 register 0x86 on its ESP32-P4 example.

References:

- https://www.waveshare.com/wiki/8-DSI-TOUCH-A
- https://www.waveshare.com/8-dsi-touch-a.htm

### 2.1 DSI connection rule

Do not route MIPI DSI through a general-purpose board-to-board cable or GPIO connector. Use the Core-DEV-KIT's intended DSI connector and the Waveshare-supported cable arrangement, or reproduce that connector/pinout exactly after checking the vendor schematics.

For a custom carrier revision, route DSI as controlled-impedance differential pairs with continuous reference plane and matched intra-pair lengths. Exact stack-up constraints belong in the KiCad board rules once the PCB fabricator is selected.

### 2.2 Touch / display control bus

The current Core profile reserves GPIO7/GPIO8 as the BSP I2C bus. The display/touch path, TCA8418 and PCA9956 devices share this bus. Do not create a second WaveX I2C controller instance on the same pins.

## 3. ESP32-P4-Core-DEV-KIT interface

The current branch allocation is based on the Waveshare carrier schematic and intentionally separates high-speed functions from panel GPIO.

### 3.1 Reserved interface allocation

| Function | GPIO | Notes |
|---|---:|---|
| BSP I2C SDA | 7 | GT9271/display control + TCA8418 + PCA9956B devices |
| BSP I2C SCL | 8 | shared bus |
| Backend UART TX | 22 | live control link during initial RT1176 port |
| Backend UART RX | 23 | live control link during initial RT1176 port |
| DIN MIDI RX | 20 | opto-isolated receiver required |
| DIN MIDI TX | 21 | buffered MIDI current-loop transmitter |
| Reserved backend SPI SCLK | 9 | SPI3 slave profile |
| Reserved backend SPI MOSI | 10 | backend → frontend |
| Reserved backend SPI MISO | 11 | frontend → backend |
| Reserved backend SPI CS | 12 | backend-controlled chip select |
| Backend ATTN | 13 | frontend → backend attention |
| Nav encoder A phases | 33, 32 | PCNT |
| Nav encoder B phases | 46, 47 | PCNT |
| TCA8418 INT | 28 | bottom pad; active-low open-drain |
| Panel SPI2 SCLK | 54 | MCP3208 only |
| Panel SPI2 MOSI | 53 | MCP3208 DIN |
| Panel SPI2 MISO | 48 | MCP3208 DOUT |
| MCP3208 CS | 36 | bottom pad |
| PCA9956B RESET | 29 | shared active-low RESET, external pull-up |
| PCA9956B OE | 30 | shared active-low OE, default blank via pull-up |

GPIO39–43 are no longer consumed by the deleted auxiliary ADC/mux path. GPIO43 returns to the free pool.

### 3.2 Pins intentionally left alone

- Native MIPI-DSI differential PHY nets are not GPIO.
- GPIO24/25 are associated with the native USB-Serial/JTAG connection in the current carrier plan; do not allocate them as panel GPIO.
- GPIO26/27 remain reserved for a possible future USB FS host port.
- GPIO34 is an onboard RGB/strap-related net on the Core carrier; leave it alone.
- GPIO35 is BOOT.
- GPIO37/38 belong to the onboard UART bridge.

## 4. Panel ADC / four RV112FF controls

Use **one MCP3208** at 3.3 V logic/reference unless later analog tests justify a separate precision reference.

Four RV112FF dual-track controls consume all eight ADC channels:

| Control | Track A | Track B |
|---|---:|---:|
| POT0 | CH0 | CH1 |
| POT1 | CH2 | CH3 |
| POT2 | CH4 | CH5 |
| POT3 | CH6 | CH7 |

Target part family: Taiwan Alpha RV112FF-40B1, dual 10 kΩ linear, 360° continuous rotation. The purchasing suffix remains subject to vendor confirmation before PCB fabrication.

Electrical rules:

- Excite both tracks from the same ADC reference and ground used by the MCP3208.
- Add local ADC decoupling per Microchip datasheet.
- Keep source impedance and input RC filtering compatible with MCP3208 acquisition settling at the selected SPI clock.
- Pair A/B traces and sample adjacent channels sequentially to bound angular skew.
- Provide test pads for every wiper on the first prototype.
- Do not add a mux between these wipers and the ADC.

## 5. Buttons and pads

Use one **TCA8418** matrix controller.

Target logical inventory is 37 keys, including:

- six soft keys,
- Shift,
- six menu-jump keys,
- Back,
- two encoder push switches,
- Track previous/next,
- Play/Stop and Record,
- sixteen performance pads.

The matrix geometry is a PCB-layout choice and must be reflected back into firmware after schematic capture. Current firmware supports up to the TCA8418's required row/column scheme and should not drive PCB topology.

Electrical rules:

- TCA8418 on shared 3.3 V I2C.
- INT is active-low/open-drain; provide pull-up and route to GPIO28.
- Encoder push switches are matrix keys, not dedicated GPIO.
- Choose switch footprints only after panel mechanical spacing is frozen.

## 6. LEDs

Use **2 × PCA9956BTWY**, 24 outputs each, 48 channels total.

- I2C addresses currently planned: 0x20 and 0x21.
- Shared active-low OE and RESET.
- OE must default inactive/high so the panel is dark until firmware initializes both devices.
- Select REXT and LED series/current configuration from the chosen LED part and desired brightness; do not copy a current value from a development board.
- Follow NXP exposed-pad thermal guidance.
- Place LED drivers near the LED groups when practical to reduce long PWM/current traces.

Current logical channel plan reserves 0–14 for soft/menu/transport LEDs and 24–39 for the sixteen pad LEDs; spare channels remain for later panel indicators.

Reference: https://www.nxp.com/docs/en/data-sheet/PCA9956B.pdf

## 7. MIDI and USB

### 7.1 DIN MIDI

UART2 is GPIO20/GPIO21.

- MIDI IN: standards-compliant isolated receiver; choose and document the exact optocoupler/digital isolator circuit before fabrication.
- MIDI OUT: buffered 3.3 V current-loop transmitter compliant with the MIDI electrical specification.
- Provide ESD protection at external connectors.

### 7.2 USB

Keep frontend USB functions separate from backend USB service/storage in the first hardware revision.

Frontend:

- native ESP32-P4 USB HS: USB MIDI/device function.
- Core-DEV-KIT programming/debug ports remain available during development.

Do not combine the frontend and backend USB ports through a hub/switch in Rev A.

## 8. Power

Required frontend rails:

- **5 V panel rail** sized for the display (0.8 A typical vendor figure, plus design margin).
- **3.3 V panel logic rail** for TCA8418, MCP3208 and PCA9956B logic/LED current supply as selected by final LED circuit.
- ESP32-P4-Core-DEV-KIT supply per Waveshare carrier schematic.

Recommended prototype strategy:

- Bring a regulated 5 V system rail to the UI board.
- Fuse/current-limit the display branch separately from the logic branch.
- Add local bulk capacitance near the display power connector and the Core carrier.
- Keep display/backlight return current away from ADC reference/wiper return paths before joining at the board ground plane.
- Add test points for 5 V, 3.3 V and ground.

The exact upstream WaveX system power tree is not yet frozen; do not fabricate the board until connector current rating and rail source are recorded in `interboard-interface.md`.

## 9. Layout guidance

- Prefer a 4-layer PCB for the frontend: signal / solid GND / power+signal / signal.
- Keep DSI cabling/connector path short and isolated from LED-current and MIDI edges.
- Place MCP3208 near the four endless controls; keep the eight wiper traces short and quiet.
- Keep the PCNT phase pairs away from LED-current traces.
- Put ESD protection at external USB/MIDI connectors.
- Place PCA9956B decoupling and REXT components immediately adjacent to each driver.
- Reserve accessible test pads for I2C, SPI2, backend UART, backend SPI and ATTN.

## 10. Fabrication blockers / prototype verification

Close these before ordering the final UI PCB:

1. Bench-verify ESP32-P4-Core-DEV-KIT boot, display and GT9271 touch with the **8-DSI-TOUCH-A**.
2. Confirm exact DSI cable/connector orientation and power harness against Waveshare drawings.
3. Confirm the Core-DEV-KIT bottom-pad mechanical footprint and solder/breakout method used by the UI PCB.
4. Scope both tracks of production-candidate RV112FF parts over a full rotation; confirm phase relationship and usable endpoint behavior.
5. Verify MCP3208 acquisition settling with the actual 10 kΩ controls and chosen RC filter.
6. Select actual panel LEDs and calculate PCA9956 current/thermal design.
7. Freeze keypad switch/pad part numbers and matrix routing.
8. Freeze DIN MIDI input/output circuit and connector type.
9. Verify shared-I2C operation with touch + keypad + two LED drivers under worst-case LED updates.
10. Perform compile-time and continuity-level GPIO collision audit after schematic net naming is complete.
