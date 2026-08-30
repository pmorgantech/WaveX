# Third-Party Licenses

This document lists all third-party libraries, frameworks, and dependencies used in the WaveX dual-MCU sampler/synthesizer project, along with their respective licenses.

## ESP32 Frontend Dependencies

### ESP-IDF Framework
- **Source**: Espressif Systems
- **License**: Apache License 2.0
- **Copyright**: Copyright 2015-2024 Espressif Systems (Shanghai) CO LTD
- **Usage**: Core framework for ESP32-P4 development
- **License File**: `/opt/esp/idf/LICENSE`

### FreeRTOS Kernel
- **Source**: Amazon Web Services / Real Time Engineers Ltd.
- **License**: MIT License
- **Copyright**: Copyright (C) 2017 Amazon.com, Inc. or its affiliates
- **Usage**: Real-time operating system for ESP32
- **License File**: `/opt/esp/idf/components/freertos/FreeRTOS-Kernel/LICENSE.md`

### LVGL (Light and Versatile Graphics Library)
- **Source**: https://github.com/lvgl/lvgl.git
- **License**: MIT License
- **Copyright**: Copyright (c) 2016 Gabor Kiss-Vamosi
- **Usage**: GUI framework for touchscreen interface
- **Fetched by**: ESP-IDF Component Manager (`lvgl/lvgl: '>=9.4,<10'` in `firmware/esp32/main/idf_component.yml`), not a git submodule
- **Vendored Path**: `firmware/esp32/managed_components/lvgl__lvgl/`
- **License File**: `firmware/esp32/managed_components/lvgl__lvgl/COPYRIGHTS.md`

#### LVGL Third-Party Components
LVGL includes several third-party libraries with their own licenses:

- **Barcode Generator**: MIT License (Code128 format)
- **Expat XML Parser**: MIT License
- **FreeType Font Rendering**: FreeType License (GPL-compatible)
- **GifDec**: MIT License
- **LodePNG**: zlib License
- **LZ4 Compression**: BSD 2-Clause License
- **QR Code Generator**: MIT License
- **ThorVG Vector Graphics**: MIT License
- **TinyTTF**: MIT License (STB-based)
- **TJPGD JPEG Decoder**: Custom license (free for commercial use)
- **TLSF Memory Allocator**: BSD 3-Clause License
- **Printf Library**: MIT License

### esp_lcd_touch_gt911 (current touch controller)
- **Source**: Espressif Systems, ESP-IDF Component Manager (`espressif/esp_lcd_touch_gt911`)
- **License**: Apache License 2.0
- **Usage**: GT911 capacitive touch controller driver — the touch controller actually fitted; supersedes XPT2046 below
- **Vendored Path**: `firmware/esp32/managed_components/espressif__esp_lcd_touch_gt911/`
- **License File**: `firmware/esp32/managed_components/espressif__esp_lcd_touch_gt911/license.txt`

### esp_lvgl_port
- **Source**: Espressif Systems, ESP-IDF Component Manager (`espressif/esp_lvgl_port`)
- **License**: Apache License 2.0
- **Usage**: LVGL/ESP-IDF display and input-device integration glue
- **Vendored Path**: `firmware/esp32/managed_components/espressif__esp_lvgl_port/`

### esp_tinyusb / tinyusb
- **Source**: Espressif Systems / hathach, ESP-IDF Component Manager
- **License**: Apache License 2.0 (`esp_tinyusb`)
- **Usage**: USB MIDI device stack
- **Vendored Path**: `firmware/esp32/managed_components/espressif__esp_tinyusb/`, `espressif__tinyusb/`

### esp_tca8418
- **Source**: https://github.com/atanisoft/esp_tca8418.git (git dependency in `idf_component.yml`)
- **License**: BSD 2-Clause License
- **Copyright**: Copyright (c) 2024, Mike Dunston
- **Usage**: TCA8418 keypad matrix controller driver
- **Vendored Path**: `firmware/esp32/managed_components/esp_tca8418/`

### Waveshare ESP32-P4-NANO BSP and panel drivers
- **Source**: Waveshare, ESP-IDF Component Manager
- **License**: Apache License 2.0
- **Usage**: Board support package (`waveshare__esp32_p4_nano`) and MIPI-DSI panel drivers (`waveshare__esp_lcd_dsi`, `waveshare__esp_lcd_hx8394`, `waveshare__esp_lcd_ili9881c`, `waveshare__esp_lcd_jd9365_10_1`, `waveshare__esp_lcd_jd9365_8`)
- **Vendored Path**: `firmware/esp32/managed_components/waveshare__*/`

### esp_codec_dev, i2c_bus, cmake_utilities
- **Source**: Espressif Systems, ESP-IDF Component Manager
- **License**: Apache License 2.0
- **Usage**: Supporting components pulled in transitively by the above
- **Vendored Path**: `firmware/esp32/managed_components/espressif__esp_codec_dev/`, `espressif__i2c_bus/`, `espressif__cmake_utilities/`

### XPT2046 Touchscreen Library (no longer used)
- **Source**: https://github.com/PaulStoffregen/XPT2046_Touchscreen.git
- **License**: MIT License
- **Copyright**: Copyright (c) 2015, Paul Stoffregen, paul@pjrc.com
- **Status**: `.gitmodules` still declares this at `firmware/esp32/libs/xpt2046`, but the path does not exist on disk and nothing in the tree references it — the GT911 driver above is what's actually fitted and used. Retained here only until the dead `.gitmodules` entry is cleaned up.

## Daisy Backend Dependencies

### libDaisy
- **Source**: https://github.com/electro-smith/libDaisy.git
- **License**: MIT License
- **Copyright**: Copyright (c) 2019 Electrosmith
- **Usage**: Hardware abstraction layer for Daisy Seed
- **Submodule Path**: `firmware/daisy/libs/libDaisy`
- **License File**: `firmware/daisy/libs/libDaisy/LICENSE`

### DaisySP
- **Source**: https://github.com/electro-smith/DaisySP.git
- **License**: MIT License
- **Copyright**: Copyright (c) 2020 Electrosmith, Corp.
- **Usage**: Digital signal processing library
- **Submodule Path**: `firmware/daisy/libs/DaisySP`
- **License File**: `firmware/daisy/libs/DaisySP/LICENSE`

#### DaisySP Third-Party Components
- **Plaits**: MIT License, Copyright 2016 Emilie Gillet (emilie.o.gillet@gmail.com)
- **Soundpipe**: MIT License, Copyright 2015 Paul Batchelor

### ARM CMSIS
- **Source**: ARM Limited
- **License**: Apache License 2.0
- **Copyright**: Copyright (c) 2009-2017 ARM Limited
- **Usage**: Cortex Microcontroller Software Interface Standard
- **License File**: `firmware/daisy/libs/libDaisy/Drivers/CMSIS_5/LICENSE.txt`

### STM32H7xx HAL Driver
- **Source**: STMicroelectronics
- **License**: BSD 3-Clause License
- **Copyright**: Copyright 2017 STMicroelectronics
- **Usage**: Hardware abstraction layer for STM32H750
- **License File**: `firmware/daisy/libs/libDaisy/Drivers/STM32H7xx_HAL_Driver/LICENSE.md`

### STM32 USB Device Library
- **Source**: STMicroelectronics
- **License**: BSD 3-Clause License
- **Copyright**: Copyright (c) 2015 STMicroelectronics
- **Usage**: USB device stack for STM32
- **License File**: `firmware/daisy/libs/libDaisy/Middlewares/ST/STM32_USB_Device_Library/LICENSE.md`

## Testing

### GoogleTest
- **Source**: https://github.com/google/googletest.git
- **License**: BSD 3-Clause License
- **Usage**: Host-based unit/integration test framework for all three test trees (`firmware/daisy/tests/`, `firmware/esp32/tests/`, `firmware/shared/tests/`) — see `docs/testing_guide.md`
- **Submodule Path**: `firmware/shared/tests/_deps/googletest-src`
- **License File**: `firmware/shared/tests/_deps/googletest-src/LICENSE`

## Development Tools

### ARM GCC Toolchain
- **License**: GPL v3+ with runtime library exception
- **Usage**: Cross-compiler for ARM Cortex-M processors

### Xtensa GCC Toolchain
- **License**: GPL v3+ with runtime library exception
- **Usage**: Cross-compiler for ESP32 processors

### CMake
- **License**: BSD 3-Clause License
- **Usage**: Build system for ESP32 firmware

### GNU Make
- **License**: GPL v3+
- **Usage**: Build system for Daisy firmware

## License Compliance

### MIT License
The majority of libraries use the MIT License, which permits:
- Commercial use
- Modification
- Distribution
- Private use

Requirements:
- Include copyright notice and license text
- Include license in all copies or substantial portions

### Apache License 2.0
Used by ESP-IDF and ARM CMSIS, permits:
- Commercial use
- Modification
- Distribution
- Patent use
- Private use

Requirements:
- Include copyright notice and license text
- State changes made to original code
- Include NOTICE file if present

### BSD 3-Clause License
Used by STMicroelectronics components, permits:
- Commercial use
- Modification
- Distribution
- Private use

Requirements:
- Include copyright notice and license text
- No endorsement using contributors' names

## License Files Location

All original license files are preserved in their respective library directories:
- ESP32 libraries: `firmware/esp32/libs/*/`
- Daisy libraries: `firmware/daisy/libs/*/`
- System libraries: `/opt/esp/idf/` and subdirectories

## Acknowledgments

We acknowledge and thank all the authors and contributors of the above libraries for their invaluable work that makes the WaveX project possible.
