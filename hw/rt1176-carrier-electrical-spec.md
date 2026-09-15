# WaveX RT1176 Backend Carrier Electrical Specification

**Status:** schematic-ready architecture with explicit pin reservations; fabrication blocked on §11 verification.

## 1. Scope

This board is the digital audio/storage/control carrier around a SomLabs **VisionSOM-RT117x / RT1176** module. It replaces the Daisy Seed role without absorbing the ESP32-P4 UI.

Target responsibilities:

- real-time audio engine,
- sample RAM,
- microSD / FatFs storage,
- stereo sampling input via AD1938,
- eight digital voice output lanes via AD1938,
- CV DAC control,
- frontend UART and future/revived SPI transport,
- USB mass-storage/service,
- JTAG/SWD/debug.

Preferred SOM population:

- RT1176,
- **128 MB SDRAM**,
- 16 MB or 32 MB QSPI flash (final density TBD),
- **no radio / 0SF variant** so SD1 and UART1-EXT pins are available.

SomLabs documents a SODIMM200 module, 5 V input, up to 128 MB SDRAM, and up to 32 MB QSPI flash.

Reference: https://wiki.somlabs.com/index.php?title=VisionSOM-RT117x_Datasheet_and_Pinout

## 2. Power and module socket

Use a 200-pin SODIMM socket footprint compatible with the VisionSOM reference carrier and SomLabs mechanical library.

### 2.1 SOM power

SomLabs pinout:

- SODIMM 41–50: VDD-5V inputs.
- SODIMM 53–54: module-generated 3.3 V output, **200 mA maximum external load**.
- SODIMM 39–40: module-generated 1.8 V output; external use should be minimized and must obey the NXP/SomLabs current limits.
- numerous GND pins must all be connected with low-impedance plane/vias.

Design rule: **do not power the analog audio board from the SOM's 3.3 V output.** Generate the codec/digital/analog rails from the system power tree on the carrier/analog board. The SOM 3.3 V output is for light local logic only.

Provide:

- 5 V bulk capacitance at SODIMM power pins,
- reset/recovery access,
- power and reset test points,
- optional backup battery footprint only if RTC/SNVS retention becomes a product requirement.

## 3. SAI1 audio interface — reserved for AD1938

Production audio uses **SAI1**. Do not assign these pins to another peripheral.

| SODIMM | Signal | RT1176 pad | Direction at backend |
|---:|---|---|---|
| 60 | SAI1.MCLK | GPIO_DISP_B2_03 | out |
| 62 | SAI1.RX | GPIO_DISP_B2_06 | in |
| 64 | SAI1.TX | GPIO_DISP_B2_07 | out |
| 66 | SAI1.FSYNC | GPIO_DISP_B2_04 | out (planned master) |
| 68 | SAI1.BCLK | GPIO_DISP_B2_05 | out (planned master) |

All are documented by SomLabs as 3.3 V-domain signals.

Target framing:

- Fs = 48 kHz.
- 8 × 32-bit TDM slots = 256 BCLK/frame.
- BCLK = 12.288 MHz.
- 24 useful data bits per slot.
- RT1176 is planned as SAI clock/frame master; AD1938 serial ports are slaves.

The exact AD1938 register format and SAI edge/polarity settings must be verified together on the bench before PCB release.

## 4. microSD / USDHC1

Use SD1 in native 4-bit mode.

| SODIMM | Signal |
|---:|---|
| 15 | SD1.DATA1 |
| 17 | SD1.DATA0 |
| 21 | SD1.CLK |
| 23 | SD1.CMD |
| 25 | SD1.DATA3 |
| 27 | SD1.DATA2 |

These signals are only available for WaveX on **SOM variants without the 1DX radio module**.

Layout:

- place microSD socket near the SOM,
- follow NXP USDHC routing/series-termination guidance,
- route CLK cleanly with controlled return path,
- add ESD protection appropriate for a user-accessible card socket,
- provide card-detect only if the selected socket and firmware use it.

The backend owns the filesystem. The ESP32-P4 never electrically owns the SD bus.

## 5. Frontend control link

Rev A keeps the proven UART path and reserves SPI separately. This avoids changing MCU and transport in the same bring-up step.

### 5.1 UART — initial/live migration link

Use LPUART1 on the no-radio SOM pins:

| SODIMM | Default name | RT1176 pad | WaveX use |
|---:|---|---|---|
| 35 | UART1-EXT.TXD | GPIO_AD_24 | backend TX → ESP32 RX |
| 33 | UART1-EXT.RXD | GPIO_AD_25 | backend RX ← ESP32 TX |
| 29 | UART1-EXT.CTS | GPIO_AD_26 | free GPIO unless flow control later |
| 31 | UART1-EXT.RTS | GPIO_AD_27 | free GPIO unless flow control later |

Initial bitrate remains 2 Mbaud until RT1176 timing is bench-verified.

### 5.2 Reserved high-speed frontend SPI

Use LPSPI4:

| SODIMM | Signal | RT1176 pad |
|---:|---|---|
| 124 | SPI4.SCK | GPIO_DISP_B2_12 |
| 122 | SPI4.OUT | GPIO_DISP_B2_14 |
| 120 | SPI4.IN | GPIO_DISP_B2_13 |
| 118 | SPI4.CS0 | GPIO_DISP_B2_15 |

RT1176 is SPI master; ESP32-P4 is SPI slave. Add one separate backend input for ESP32 **ATTN** using a free 3.3 V GPIO selected during schematic capture; reserve a nearby ground in the board-to-board connector.

Place optional source-series resistor footprints (DNP/default TBD) on SCK and master-data-out for signal-integrity tuning.

## 6. CV DAC SPI — LPSPI1

Use LPSPI1 independently from the frontend link.

NXP documents ALT0 on GPIO_AD_28..31:

| SODIMM | Pad | LPSPI1 use |
|---:|---|---|
| 34 | GPIO_AD_28 | SCK |
| 32 | GPIO_AD_29 | PCS0 / DAC0 CS |
| 30 | GPIO_AD_30 | SDO → DAC SDI |
| 28 | GPIO_AD_31 | SDI ← DAC SDO/readback |

Target DAC population: **4 × MCP48CVB28**, octal 12-bit volatile SPI DACs, 32 outputs total. Prefer the volatile `CVB` part over the `CMB` MTP part so calibration/defaults are loaded from normal storage at boot and no 7.5 V MTP-programming path is required.

Microchip specifies 24-bit SPI command boundaries, up to 50 MHz writes / 25 MHz reads, and two latch inputs per octal device. LAT0 updates even channels 0/2/4/6; LAT1 updates odd channels 1/3/5/7.

### 6.1 DAC discrete-control reservation

Four devices require four CS signals. Proposed GPIO use:

- DAC0_CS: native PCS0, SODIMM32 / GPIO_AD_29.
- DAC1_CS: SODIMM29 / GPIO_AD_26 if UART flow control is not used.
- DAC2_CS: SODIMM31 / GPIO_AD_27 if UART flow control is not used.
- DAC3_CS: choose one free 3.3 V GPIO during final pin audit; GPIO_AD_34/SODIMM20 is a current candidate.

Reserve **two shared latch controls** for all DACs:

- DAC_LAT_EVEN → all LAT0 pins.
- DAC_LAT_ODD → all LAT1 pins.

GPIO_AD_35/SODIMM22 and another free 3.3 V GPIO are candidates. Final assignment must be checked against all carrier functions before layout.

The latch architecture allows all 32 output registers to be loaded over SPI and then updated synchronously by lane group.

## 7. AD1938 control SPI

AD1938 uses SPI control. Two acceptable architectures remain:

1. share LPSPI1 SCK/SDO/SDI and give AD1938 its own CS, or
2. use another low-rate SPI instance if routing/layout makes separation cleaner.

Preference: **share LPSPI1** unless bench testing reveals a reason not to. Codec register writes are infrequent and do not justify another bus.

Reserve one additional codec CS GPIO and the AD1938 active-low reset/power-down GPIO.

## 8. USB

### 8.1 USB1 — backend service / mass storage

Reserve USB1 for the user-facing backend service/MSC port.

| SODIMM | Signal |
|---:|---|
| 63 | USB1.D_N |
| 65 | USB1.D_P |
| 67 | USB1.VBUS |
| 69 | USB1.OC |
| 71 | USB1.EN |
| 73 | USB1.ID |

Use a USB-C or other selected connector with correct CC configuration, VBUS sensing/power switching and ESD protection according to the intended host/device/OTG role.

MSC policy is exclusive ownership of the SD filesystem: unmount FatFs before exposing raw media to a USB host.

### 8.2 USB2 — reserve

USB2 is available on SODIMM 77/79/81/83/85/87. Keep it uncommitted in Rev A unless a concrete host function is needed.

## 9. Debug / recovery

Expose JTAG/SWD and recovery on the carrier even if the development carrier has onboard DAP.

Relevant SODIMM pins include:

- 3 JTAG.TMS,
- 5 JTAG.TCK,
- 7 JTAG.TDO,
- 9 JTAG.TDI,
- 11 JTAG.TRST,
- 13 JTAG.MOD,
- 57 RECOVERY,
- 10 RESET,
- 12 RESET-IN.

Respect the 1.75 V SNVS-domain restriction on system-control lines; do not treat RESET/ON-OFF/WAKEUP as ordinary 3.3 V GPIO without checking SomLabs/NXP requirements.

Use a standard debug connector compatible with the selected probe, plus test pads for recovery and reset.

## 10. Carrier-to-analog-board interface

The digital carrier should send only digital/control signals and power/control references to the analog voice board; it should not carry eight single-ended audio channels across a long general-purpose harness if the AD1938 is located on the analog board.

Preferred partition:

- AD1938 lives on **analog voice/audio board**.
- SAI1 MCLK/BCLK/FSYNC/TX/RX cross the board-to-board connector.
- CV SPI SCK/SDO/SDI, CS lines and LAT lines cross the connector.
- codec SPI CS/reset cross the connector.
- analog board has its own clean 3.3 V analog/digital regulation and ± analog rails.
- generous ground pins accompany every clock/control group.

This keeps codec analog paths, DAC outputs, filters and mixer on one PCB.

## 11. Fabrication blockers / verification

Before release:

1. Confirm exact VisionSOM order code and 128 MB/no-radio availability.
2. Import/verify SomLabs SODIMM200 symbol/footprint against its published Altium library and STEP model.
3. Complete NXP IOMUX audit for every non-default GPIO used as DAC CS/LAT/ATTN/reset.
4. Bench-bring up SAI1 TDM TX/RX on VisionCB using the onboard codec or logic analyzer before committing the custom carrier.
5. Verify LPSPI1 on GPIO_AD_28..31 with eDMA at the intended clock.
6. Verify LPSPI4 master ↔ ESP32-P4 slave and ATTN timing before making SPI the production transport.
7. Select and verify USB-C role/power circuitry.
8. Select the microSD socket and verify USDHC signal-integrity/ESD network.
9. Freeze the system power tree and carrier input connector/current rating.
10. Run a complete SODIMM-pin conflict report before layout starts.

## 12. Primary references

- SomLabs VisionSOM RT117x pinout: https://wiki.somlabs.com/index.php?title=VisionSOM-RT117x_Datasheet_and_Pinout
- SomLabs VisionCB RT1176 STD reference carrier: https://wiki.somlabs.com/index.php/VisionCB-RT1176-STD_v.1.2_Datasheet_and_Pinout
- NXP i.MX RT1170 product/reference manuals: https://www.nxp.com/products/i.MX-RT1170
- Microchip MCP48CXBX4/8 datasheet: https://ww1.microchip.com/downloads/en/DeviceDoc/MCP48CXBX4_8-Family-Data-Sheet-20006556B.pdf
