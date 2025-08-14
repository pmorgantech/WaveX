# WaveX - Dual-MCU Sampler/Synth

A high-performance dual-MCU sampler and synthesizer featuring an ESP32-S3 frontend and Daisy Seed (STM32H750) backend.

## Architecture Overview

**WaveX** implements a dual-microcontroller architecture optimizing each MCU for its specific role:

### Backend - Audio Engine (Daisy Seed/STM32H750)
- **Real-time audio processing** at 48kHz/24-bit
- **Sample playback engine** with interpolation and loop support
- **Synthesis modules**: Envelopes, LFOs, filters, modulation matrix
- **CV outputs** via SPI DACs for modular integration
- **High-performance DSP** using ARM Cortex-M7 at 480MHz

### Frontend - User Interface (ESP32-S3)
- **Touchscreen interface** using LVGL graphics library
- **File management** with SD card and USB connectivity
- **MIDI I/O** for external controller integration
- **User controls** and parameter adjustment
- **Sample management** and preset storage

### Inter-MCU Communication
- **High-speed SPI protocol** for control data and sample transfer
- **Custom protocol** optimized for real-time audio applications
- **Shared library** for protocol definitions and utilities

## Project Structure

```
WaveX/
├── firmware/
│   ├── esp32/                 # ESP32-S3 Frontend Firmware
│   │   ├── main/             # Main application code
│   │   ├── components/       # Custom ESP-IDF components
│   │   └── libs/             # External libraries (LVGL, etc.)
│   ├── daisy/                # Daisy Seed Backend Firmware
│   │   ├── src/              # Audio engine source code
│   │   └── libs/             # libDaisy, DaisySP libraries
│   └── shared/               # Shared protocol library
│       ├── spi_protocol/     # Inter-MCU communication protocol
│       └── utils/            # Common utilities
├── .devcontainer/            # Development container configuration
├── build.sh                 # Build script for both firmwares
└── Makefile                  # Convenient build targets
```

## Current Status

### ✅ Completed
- **Development Environment**: Devcontainer with ESP-IDF 5.2, ARM GCC toolchain, and reliable USB device passthrough
- **Build System**: Unified Makefile and `idf.py` build successful for ESP32 with unnecessary components disabled.
- **ESP32 Firmware**: Project structure, builds successfully with zero warnings
- **Hardware Compatibility**: All GPIO assignments verified for ESP32-S3-DevKitC-1
- **Display Driver**: ST7796S 480x320 TFT display support via `esp_lcd` and `esp_lvgl_port`.
- **Touch Integration**: FT6X36 I2C capacitive touch with LVGL integration.
- **Shared Protocol Library**: SPI communication protocol definitions
- **Git Submodules**: LVGL, libDaisy, DaisySP properly configured
- **Documentation**: Architecture and setup documentation updated.

### 🔄 In Progress
- **Daisy Firmware**: Resolving build issues with HAL dependencies.
- **Protocol Implementation**: Complete SPI communication handlers
- **UI Migration**: Migrating from LVGL to LovyanGFX-based custom UI (see `docs/WaveX_UI_Redesign.md`).

### 📋 Next Steps
- Resolve libDaisy compilation issues or use alternative STM32 framework
- Create basic audio engine structure for Daisy
- Add sample loading and playback functionality
- Implement real-time parameter communication between MCUs
- Test hardware integration with ESP32-S3-DevKitC-1

## Quickstart (Devcontainer)

1. **Clone the repo**
2. Open in VS Code with the [Dev Containers extension](https://code.visualstudio.com/docs/devcontainers/containers)
3. Hit F1 → "Reopen in Container"
4. **Build both systems**: `make all` (builds ESP32 + Daisy with enhanced visual output)
5. **Flash firmware**: `make esp32-flash` and flash Daisy via DFU
6. Follow the [setup guide](./setup.md) for detailed build, flash, and workflow instructions

## Build System

WaveX uses a dual build system approach optimized for each platform:

- **ESP32 Frontend**: CMake with ESP-IDF's `idf.py` (component-based, integrated toolchain)
- **Daisy Backend**: Traditional Make with libDaisy templates (optimized STM32 build)
- **Orchestration**: Top-level Makefile with enhanced visual output

**Quick Commands:**
```bash
make all        # Build both ESP32 and Daisy with clear progress
make esp32      # Build ESP32 frontend only
make daisy      # Build Daisy backend only
make clean      # Clean both build systems
make setup      # Initialize git submodules
```

## Hardware Requirements

### ESP32-S3 Frontend
- ESP32-S3 with at least 8MB PSRAM
- 3.5" TFT display (ST7796S controller)
- FT6x36 capacitive touch controller (I2C)
- SD card interface
- USB MIDI interface
- SPI connection to Daisy

### Daisy Seed Backend
- Daisy Seed (STM32H750VBT6)
- Audio I/O connections
- SPI DAC for CV outputs
- SPI connection to ESP32

## Hardware Pinout & Wiring

### ESP32-S3 Frontend Pin Assignments
**⚠️ All pin assignments verified for ESP32-S3-DevKitC-1 compatibility**

#### 🖥️ ST7796S Display Controller (480x320 TFT)
```
Display Pin     Signal          GPIO    Pin Location    Description
-----------     ------          ----    ------------    -----------
1               VCC             5V      Power          LCD power positive (5V recommended)
2               GND             GND     Power          LCD power ground
3               LCD_CS          5       J1 pin 5       LCD chip select (low active)
4               LCD_RST         2       J3 pin 5       LCD reset (low reset)
5               LCD_RS (DC)     4       J1 pin 4       Data/Command control
6               SDI (MOSI)      6       J1 pin 6       SPI Data Out
7               SCK             7       J1 pin 7       SPI Clock (40MHz)
8               LED (Backlight) 21      J3 pin 18      Backlight control
9               SDO (MISO)      19      J3 pin 20      SPI Data In (optional)
```

#### 👆 I2C Capacitive Touch Panel
```
Touch Pin       Signal          GPIO    Pin Location    Description
---------       ------          ----    ------------    -----------
10              CTP_SCL         9       J3 pin 4       I2C Clock for touch controller
11              CTP_SDA         20      J3 pin 19      I2C Data for touch controller
12              CTP_INT         -1      -              Interrupt pin (optional, disabled in config)
```

**✅ Touch Technology**: Firmware supports FT6x36 I2C capacitive touch controllers (FT6236/FT6336). LVGL integration exists today; migration to LovyanGFX touch handling is planned.

**📡 SPI Configuration**: Multiple independent SPI interfaces are used - Display (VSPI), Inter-MCU (custom), and SD Card (HSPI) for optimal performance and signal isolation.

#### 🔗 Inter-MCU Communication (ESP32 ↔ Daisy)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
SPI_CS          8       J1 pin 12      SPI Chip Select to Daisy
SPI_SCLK        18      J1 pin 11      SPI Clock (10MHz max for audio timing)
SPI_MOSI        47      J3 pin 17      SPI Data to Daisy (control messages)
SPI_MISO        37      —               SPI Data from Daisy (status/feedback)
IRQ_IN          16      —               Daisy→ESP32 interrupt (from PB0)
```

#### 💾 SD Card Interface
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
SD_CS           10      J1 pin 16      SD Card Chip Select
SD_SCLK         12      J1 pin 18      SD Card SPI Clock
SD_MOSI         11      J1 pin 17      SD Card Data Out
SD_MISO         13      J1 pin 19      SD Card Data In
```

#### 🎵 MIDI Interface
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
MIDI_TX         17      J1 pin 10      MIDI UART TX (5-pin DIN out)
MIDI_RX         42      J3 pin 6       MIDI UART RX (5-pin DIN in)
USB_D+          -       Built-in       USB MIDI Data+ (built-in USB)
USB_D-          -       Built-in       USB MIDI Data- (built-in USB)
```

### Daisy Seed Backend Pin Assignments

#### 🎧 Audio I/O (Built-in AK4556 Codec)
```
Signal          Pin     Description
------          ---     -----------
AUDIO_IN_L      A/D_L   Left audio input (line/instrument)
AUDIO_IN_R      A/D_R   Right audio input  
AUDIO_OUT_L     D/A_L   Left audio output
AUDIO_OUT_R     D/A_R   Right audio output
```

#### 🔗 Inter-MCU Communication (Daisy ↔ ESP32)
```
Signal          Pin     Description
------          ---     -----------
SPI_CS          D7      SPI Chip Select from ESP32
SPI_SCLK        D8      SPI Clock from ESP32
SPI_MOSI        D9      SPI Data from ESP32 (parameter updates)
SPI_MISO        D10     SPI Data to ESP32 (status/audio meters)
```

#### 🎛️ CV Outputs (via SPI DACs)
```
Signal          Pin     Description
------          ---     -----------
CV_OUT_1        D22     CV Output 1 (envelope, LFO, etc.)
CV_OUT_2        D23     CV Output 2
GATE_OUT        D24     Gate/Trigger Output
DAC_CS          D25     SPI DAC Chip Select
DAC_SCLK        D26     SPI DAC Clock
DAC_MOSI        D27     SPI DAC Data
```

#### 🎚️ Analog Inputs (for hardware controls)
```
Signal          Pin     Description
------          ---     -----------
CTRL_1          A0      Potentiometer/CV Input 1
CTRL_2          A1      Potentiometer/CV Input 2  
CTRL_3          A2      Potentiometer/CV Input 3
CTRL_4          A3      Potentiometer/CV Input 4
```

### Complete System Wiring Diagram

```
┌─────────────────┐                    ┌─────────────────┐
│    ESP32-S3     │◄───  SPI Bus  ────►│  Daisy Seed     │
│   (Frontend)    │                    │   (Backend)     │
│                 │                    │                 │
│ ┌─────────────┐ │                    │ ┌─────────────┐ │
│ │   ST7796S   │ │                    │ │   AK4556    │ │
│ │   Display   │ │                    │ │   Codec     │ │
│ │  480x320    │ │                    │ │ 48kHz/24bit │ │
│ └─────────────┘ │                    │ └─────────────┘ │
│                 │                    │                 │
│ ┌─────────────┐ │                    │ ┌─────────────┐ │
│ │ Capacitive  │ │                    │ │  SPI DACs   │ │
│ │ Touch (I2C) │ │                    │ │ CV Outputs  │ │
│ └─────────────┘ │                    │ └─────────────┘ │
│                 │                    │                 │
│ ┌─────────────┐ │                    │                 │
│ │   SD Card   │ │                    │                 │
│ │   Storage   │ │                    │                 │
│ └─────────────┘ │                    │                 │
└─────────────────┘                    └─────────────────┘
        │                                       │
        ▼                                       ▼
   ┌──────────┐                          ┌──────────┐
   │   USB    │                          │  Audio   │
   │   MIDI   │                          │   I/O    │
   └──────────┘                          └──────────┘
```

### Power Requirements

#### ESP32-S3 Frontend
- **Voltage**: 3.3V
- **Current**: ~200mA (display active) / ~50mA (sleep)
- **Power**: USB 5V → 3.3V regulator

#### Daisy Seed Backend  
- **Voltage**: 3.3V
- **Current**: ~150mA (audio processing)
- **Power**: USB 5V → 3.3V regulator or direct 3.3V supply

### Communication Protocol Timing

The SPI communication between MCUs operates with specific timing constraints:

```
Protocol        Clock       Max Data Rate    Latency
--------        -----       -------------    -------
Inter-MCU SPI   10MHz       1.25MB/s        <1ms
Audio Callback  48kHz       64 samples       1.33ms
Parameter Sync  1kHz        Control rate     1ms
```

### Development/Debug Interfaces

#### ESP32-S3 Debug
- **USB-Serial**: Built-in USB-to-Serial for programming/debug
- **JTAG**: Standard 20-pin JTAG for hardware debugging

#### Daisy Seed Debug  
- **USB-DFU**: Built-in DFU bootloader for programming
- **SWD**: 4-pin SWD interface for debugging
- **UART**: Serial debug output via USB

## Hardware Pin Documentation

Based on the provided LCD/touch controller specs, here are the key signals and their behaviors, mapped to ESP32-S3 pins (from `hardware_pins.h`):

### LCD Signals
| Signal | Description | Active Level | ESP32 GPIO | Notes |
|--------|-------------|--------------|------------|-------|
| LCD_CS | LCD selection control | Low active | GPIO5 | Handled by SPI driver |
| LCD_RST | LCD reset control | Low reset | GPIO2 | Assert low to reset, high to release |
| LCD_RS (DC) | Command/Data select | High: data, Low: command | GPIO4 | Configured in panel IO |
| LED Backlight | PWM backlight control | PWM duty cycle | GPIO21 | Uses LEDC for brightness (0-255) |
| SDI (MOSI) | SPI data in | N/A | GPIO6 | Display data input |
| SCK | SPI clock | N/A | GPIO7 | 40MHz clock |
| SDO (MISO) | SPI data out | N/A | GPIO19 | Optional, for readback |

### Touch Signals
| Signal | Description | Active Level | ESP32 GPIO | Notes |
|--------|-------------|--------------|------------|-------|
| CTP_SCL | I2C clock | N/A | GPIO9 | Touch clock signal |
| CTP_SDA | I2C data | N/A | GPIO20 | Touch data signal |
| CTP_INT | Touch interrupt | Low on touch | GPIO15 | Falling-edge interrupt; enable pull-up |
| CTP_RST | Touch reset | Low reset | GPIO14 | Assert low to reset |

**Troubleshooting Tips**:
- **GPIO Stability**: If pin levels don't hold (e.g., reset reads 0 after set to 1), enable internal pull-ups in `ui_main.cpp` GPIO config. For stronger pull, add external 4.7kΩ resistors to 3.3V.
- **Interrupt Mode**: Touch uses interrupt-driven polling for efficiency; verify CTP_INT pulls low on touch with multimeter.
- **PWM Backlight**: Brightness controlled via LEDC (5kHz); test with duty cycle 128 (~50% brightness).
- **Verification**: After init, check logs for pin states (e.g., "Reset pin state: 1" for deasserted).

For full pinout and wiring diagram, see the "Hardware Pinout & Wiring" section below.

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [Electro-Smith](https://www.electro-smith.com/) for the excellent Daisy platform and libraries
- [LVGL](https://lvgl.io/) for the embedded graphics library
- [Espressif](https://www.espressif.com/) for the ESP32 platform and ESP-IDF
- All the contributors who have invested their time and effort into this project.

## ESP32-S3 Pin Assignments

WaveX uses an ESP32-S3-DevKitC-1 as the frontend MCU with the following verified pin assignments:

### 🖥️ ST7796S Display Controller (480x320 TFT)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| SPI Clock (SCLK) | GPIO7 | J1-7 | ✅ Verified |
| SPI MOSI | GPIO6 | J1-6 | ✅ Verified |
| Chip Select (CS) | GPIO5 | J1-5 | ✅ Verified |
| Data/Command (DC) | GPIO4 | J1-4 | ✅ Verified |
| Reset (RST) | GPIO2 | J3-5 | ✅ Verified |
| Backlight (BL) | GPIO21 | J3-18 | ✅ Verified |

### 👆 Touch Controller (FT6X36 I2C)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| I2C Data (SDA) | GPIO20 | J3-19 | ✅ Verified |
| I2C Clock (SCL) | GPIO9 | J1-13 | ✅ Default I2C SCL |
| Reset (RST) | GPIO14 | J1-20 | 🔧 Updated from GPIO15 |

### 🔗 Inter-MCU Communication (ESP32 ↔ Daisy)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| SPI Chip Select | GPIO8 | J1-12 | ✅ Verified |
| SPI Clock | GPIO18 | J1-11 | ✅ Verified |
| SPI MOSI | GPIO47 | J3-17 | ✅ Verified |
| SPI MISO | GPIO37 | — | 🔧 Remapped (GPIO19 in use) |
| IRQ from Daisy | GPIO16 | — | ✅ Verified |

### 💾 SD Card Interface
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| SPI Chip Select | GPIO10 | J1-16 | ✅ Verified |
| SPI Clock | GPIO12 | J1-18 | ✅ Verified |
| SPI MOSI | GPIO11 | J1-17 | ✅ Verified |
| SPI MISO | GPIO13 | J1-19 | ✅ Verified |

### 🎵 MIDI Interface
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| UART TX | GPIO17 | J1-10 | ✅ Verified |
| UART RX | GPIO42 | J3-6 | ✅ Verified |

### 📌 Pin Usage Summary

**Used Pins:** GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO8, GPIO9, GPIO10, GPIO11, GPIO12, GPIO13, GPIO14, GPIO16, GPIO17, GPIO18, GPIO19, GPIO20, GPIO21, GPIO37, GPIO42, GPIO47

**Available Pins:** GPIO0, GPIO1, GPIO3, GPIO15, GPIO22-25, GPIO33-36, GPIO38-48 (excluding used pins)

**Reserved/Avoid:** GPIO26-32 (SPI Flash/PSRAM), GPIO45-46 (Strapping pins)

### 🎯 Pin Selection Rationale

**Display Pins (GPIO2, 4-7, 21):** Grouped together for efficient SPI communication and signal integrity.

**Touch I2C (GPIO9, 20):** GPIO9 is the default I2C SCL pin on ESP32-S3, GPIO20 is default SDA alternative.

**Inter-MCU SPI (GPIO8, 18-19, 47):** Uses SPI2 bus with high-speed capable pins for audio data transfer.

**SD Card (GPIO10-13):** Dedicated SPI bus to avoid conflicts with display and inter-MCU communication.

**Design Principles:**
- ✅ No strapping pin conflicts (GPIO0, 3, 45, 46 avoided for critical functions)
- ✅ SPI Flash/PSRAM pins (GPIO26-32) completely avoided
- ✅ Grouped related functions on same SPI buses
- ✅ Reserved ADC pins (GPIO1, 15, 16) for future analog sensors
- ✅ Touch-capable pins available for future capacitive controls

### 🔧 Hardware Validation Status

**✅ Verified Components:**
- ST7796S Display: Full initialization and LVGL integration working
- FT6x36 Touch Controller: I2C communication and touch event handling
- Inter-MCU SPI: Command/data exchange with Daisy Seed
- SD Card Interface: Ready for data logging and preset storage

**🎯 Target Board:** ESP32-S3-DevKitC-1 (N8R8 PSRAM variant recommended)

**📚 Reference:** Pin assignments verified against [ESP32-S3 DevKitC Official Pinout](https://randomnerdtutorials.com/esp32-s3-devkitc-pinout-guide/)
