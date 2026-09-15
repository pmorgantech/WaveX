# WaveX Hardware Design

**Status:** electrical architecture / KiCad capture input.

This directory is the source of truth for board-level electrical design. Firmware documents may describe behavior and ownership, but component wiring, board partitioning, power, connector contracts, and hardware pin allocations belong here.

## Board partition

1. **Frontend / HMI** — ESP32-P4-Core-DEV-KIT, Waveshare 8-DSI-TOUCH-A, panel controls, LEDs, MIDI.
2. **Digital backend carrier** — SomLabs VisionSOM-RT117x / RT1176, microSD, USB service/MSC, debug, frontend link, SAI/SPI connection to analog board.
3. **Analog voice/audio board** — AD1938, CV DACs, eight analog filter lanes, analog gain/pan/summing, sampling input and audio outputs.
4. **Optional jack board** — passive/mechanical breakout only when enclosure geometry prevents placing jacks directly on the analog/frontend boards.

The first KiCad capture should keep these as independent designs rather than one large mixed-signal PCB.

## Current electrical documents

- [`frontend-ui-electrical-spec.md`](frontend-ui-electrical-spec.md)
- [`rt1176-carrier-electrical-spec.md`](rt1176-carrier-electrical-spec.md)
- [`analog-voice-electrical-spec.md`](analog-voice-electrical-spec.md)
- [`interboard-interface.md`](interboard-interface.md)

## Source-of-truth rules

- Do not copy old Daisy or ESP32-P4-WIFI6 pin tables into new schematics.
- The production frontend has **no Wi-Fi or Bluetooth requirement**.
- The frontend analog controls are exactly **four dual-track RV112FF controls feeding one MCP3208**. There is no planned second MCP3208 or 74HC4067.
- The display target is **Waveshare 8-DSI-TOUCH-A**. The previous 5-inch and 7-inch references are historical.
- The backend target is **VisionSOM-RT117x / RT1176**, preferably a no-radio 128 MB SDRAM variant so SD1 is available.
- Vendor schematics, datasheets and the NXP/Espressif reference manuals win over historical WaveX documentation.
- Any `TBD` marked as a fabrication blocker must be closed before Gerbers are released.

## KiCad organization

When capture begins, place projects under `hw/kicad/`, for example:

```text
hw/kicad/
  wavex-frontend/
  wavex-backend-carrier/
  wavex-analog-voice/
  wavex-jack-board/        # only if mechanically required
```

Prefer hierarchical sheets by subsystem: power, processor/module, display/panel, inter-board I/O, storage/USB, codec, CV DACs, filter lanes, mixer/output.

## Primary vendor references

- Waveshare ESP32-P4-Core-DEV-KIT: https://docs.waveshare.com/ESP32-P4-Core-DEV-KIT
- Waveshare 8-DSI-TOUCH-A: https://www.waveshare.com/wiki/8-DSI-TOUCH-A
- Espressif ESP32-P4 documentation: https://www.espressif.com/en/support/documents/technical-documents
- SomLabs VisionSOM-RT117x datasheet/pinout: https://wiki.somlabs.com/index.php?title=VisionSOM-RT117x_Datasheet_and_Pinout
- NXP i.MX RT1170 reference manual/data sheet: https://www.nxp.com/products/i.MX-RT1170
