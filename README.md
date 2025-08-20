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
- **High-speed UART protocol** (230.4kbps) for control data and sample transfer
- **Custom protocol** optimized for real-time audio applications with packet statistics
- **Shared library** for protocol definitions and utilities
- **Bidirectional communication** with comprehensive testing and monitoring
- **Automatic test messages** for continuous communication verification
- **Packet statistics tracking** for debugging and performance analysis

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
- **Shared Protocol Library**: UART communication protocol definitions and implementations
- **Inter-MCU Communication**: Full bidirectional UART protocol with packet statistics and testing
- **Daisy Firmware**: QSPI flash configuration, automated build/flash process
- **Git Submodules**: LVGL, libDaisy, DaisySP properly configured
- **Documentation**: Architecture and setup documentation updated.

### 🔄 In Progress
- **Protocol Implementation**: Complete UART communication handlers and message routing
- **Audio Engine**: Basic audio processing framework on Daisy backend
- **UI Migration**: Migrating from LVGL to LovyanGFX-based custom UI (see `docs/WaveX_UI_Redesign.md`).

### 📋 Next Steps
- Complete audio engine structure for Daisy with sample playback
- Add sample loading and management functionality
- Implement real-time parameter synchronization between MCUs
- Test hardware integration with complete dual-MCU setup
- Add file system support for sample storage

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
- Audio I/O connections (AK4556 codec)
- **SD Card support** via SPI interface
- **Multiple MCP4728 DACs** for CV outputs
- **PCM1690 DAC** on SAI2 interface
- **USB support** for storage and device mode
- UART connection to ESP32

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

**📡 SPI Configuration**: Multiple independent SPI interfaces are used - Display (VSPI), SD Card (HSPI), and LED Driver (HSPI) for optimal performance and signal isolation.

#### 🎛️ Dual Potentiometer Interface (CD74HC4067)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
ADDR_A0         33      J3 pin 7       Address select bit 0
ADDR_A1         34      J3 pin 8       Address select bit 1
ADDR_A2         35      J3 pin 9       Address select bit 2
ADDR_A3         36      J3 pin 10      Address select bit 3
ENABLE          37      J3 pin 11      Enable (active low)
SIGNAL          1       ADC1_CH0       Common analog signal input
```
**✅ Technology**: 16-channel analog multiplexer supporting dual potentiometers (RV112FF-40-15A-0B10K) with 12-bit ADC resolution

#### 🔘 Button Matrix Interface (TCA8418)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
I2C_SDA         39      J3 pin 12      I2C data line
I2C_SCL         40      J3 pin 13      I2C clock line
RESET           41      J3 pin 14      Hardware reset
INTERRUPT       43      J3 pin 15      Touch interrupt (active low)
```
**✅ Technology**: 8x8 capacitive touch button matrix with I2C interface and interrupt support

#### 💡 LED Driver Interface (TLC5947)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
SPI_SCLK        44      J3 pin 16      SPI clock
SPI_MOSI        45      J3 pin 17      SPI data
SPI_CS          46      J3 pin 18      Chip select
BLANK           47      J3 pin 19      Blank control
LATCH           48      J3 pin 20      Latch control
```
**✅ Technology**: 48-channel 12-bit PWM LED driver with SPI interface for status indicators and backlighting

#### 🔗 Inter-MCU Communication (ESP32 ↔ Daisy)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
UART_TX         17      J1 pin 10      UART1 TX to Daisy (control messages)
UART_RX         18      J1 pin 11      UART1 RX from Daisy (status/feedback)
GND             GND     —               Common ground
```
**Note**: Uses UART1 for reliable bidirectional communication at 230.4kbps with comprehensive packet statistics and automatic testing

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
MIDI_TX         8       J1 pin 12      MIDI UART TX (5-pin DIN out)
MIDI_RX         42      J3 pin 6       MIDI UART RX (5-pin DIN in)
USB_D+          -       Built-in       USB MIDI Data+ (built-in USB)
USB_D-          -       Built-in       USB MIDI Data- (built-in USB)
```
**Note**: UART2 used for DIN MIDI, built-in USB for USB MIDI

### Daisy Seed Backend Pin Assignments

**⚠️ IMPORTANT**: All pin assignments verified against actual Daisy Seed hardware (D0-D30 only available)

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
UART_TX         D0      UART4 TX to ESP32 (status/audio meters)
UART_RX         D1      UART4 RX from ESP32 (parameter updates)
GND             GND     Common ground
```
**Note**: Uses UART4 for reliable bidirectional communication at 230.4kbps with comprehensive packet statistics and automatic testing

#### 🎛️ CV Outputs (via MCP4728 DACs)
```
Signal          Pin     Description
------          ---     -----------
DAC1_CS         D25     MCP4728 #1 Chip Select
DAC2_CS         D26     MCP4728 #2 Chip Select
DAC3_CS         D27     MCP4728 #3 Chip Select
DAC4_CS         D28     MCP4728 #4 Chip Select
DAC_SCLK        D29     SPI Clock (shared)
DAC_MOSI        D30     SPI Data (shared)
```
**✅ Technology**: 4x MCP4728 12-bit DACs providing 16 CV outputs total

**⚠️ Note**: Daisy Seed only exposes D0-D30 pins. D31+ do not exist on the hardware.

**✅ PIN CONFLICTS RESOLVED**:
- **D10-D11**: Now used for inter-MCU UART4 communication
- **D2-D6**: Available for other functions (was SDMMC1)
- **D17-D20**: Used for SPI SD card interface
- **D21-D24**: Available for PCM1690 SAI2 interface

**💡 SOLUTION**: Using SPI SD cards eliminates hardware conflicts and provides flexible pin assignment.

**🎯 NEWLY AVAILABLE PINS**:
- **D2-D6**: Available for additional functions (was SDMMC1)
- **D7-D8**: Available for other interfaces (was inter-MCU UART)
- **D9-D16**: Available for additional peripherals

#### 🎵 High-Quality Audio Output (PCM1690)
```
Signal          Pin     Description
------          ---     -----------
PCM_BCLK        D24     SAI2 Bit Clock
PCM_LRCK        D23     SAI2 Left/Right Clock
PCM_DATA        D22     SAI2 Data
PCM_MCLK        D21     SAI2 Master Clock
```
**✅ Technology**: PCM1690 24-bit/192kHz DAC for high-fidelity audio output
**✅ Technology**: PCM1690 24-bit/192kHz DAC for high-fidelity audio output

#### 💾 SD Card Interface (SPI)
```
Signal          Pin     Description
------          ---     -----------
SD_CS           D19     SD Card Chip Select
SD_SCLK         D20     SD Card SPI Clock
SD_MOSI         D18     SD Card Data Out
SD_MISO         D17     SD Card Data In
```
**✅ Technology**: SPI interface for standard SD cards - flexible pin assignment

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
│    ESP32-S3     │◄───  UART Bus  ────►│  Daisy Seed     │
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

The UART communication between MCUs operates with specific timing constraints:

```
Protocol        Clock       Max Data Rate    Latency    Features
--------        -----       -------------    -------    --------
Inter-MCU UART  230.4kbps   230.4kb/s       <5ms       Packet stats, auto-testing, throttled logging
Audio Callback  48kHz       64 samples       1.33ms     Real-time audio processing
Parameter Sync  1kHz        Control rate     1ms        MIDI and control updates
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

WaveX uses an ESP32-S3-DevKitC-1 as the frontend MCU with the following comprehensive pin assignments:

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
| UART TX | GPIO17 | J1-10 | ✅ Updated to UART1 |
| UART RX | GPIO18 | J1-11 | ✅ Updated to UART1 |
| GND | GND | — | ✅ Common ground |

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
| UART TX | GPIO8 | J1-12 | ✅ Updated to UART2 |
| UART RX | GPIO42 | J3-6 | ✅ Verified |

### 🎛️ Dual Potentiometer Interface (CD74HC4067)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| Address A0 | GPIO33 | J3-7 | 🆕 New |
| Address A1 | GPIO34 | J3-8 | 🆕 New |
| Address A2 | GPIO35 | J3-9 | 🆕 New |
| Address A3 | GPIO36 | J3-10 | 🆕 New |
| Enable | GPIO37 | J3-11 | 🆕 New |
| Signal | GPIO1 | ADC1_CH0 | 🆕 New |

### 🔘 Button Matrix Interface (TCA8418)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| I2C SDA | GPIO39 | J3-12 | 🆕 New |
| I2C SCL | GPIO40 | J3-13 | 🆕 New |
| Reset | GPIO41 | J3-14 | 🆕 New |
| Interrupt | GPIO43 | J3-15 | 🆕 New |

### 💡 LED Driver Interface (TLC5947)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| SPI SCLK | GPIO44 | J3-16 | 🆕 New |
| SPI MOSI | GPIO45 | J3-17 | 🆕 New |
| SPI CS | GPIO46 | J3-18 | 🆕 New |
| Blank | GPIO47 | J3-19 | 🆕 New |
| Latch | GPIO48 | J3-20 | 🆕 New |

### 📌 Pin Usage Summary

**Used Pins:** GPIO1, GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO8, GPIO9, GPIO10, GPIO11, GPIO12, GPIO13, GPIO14, GPIO15, GPIO17, GPIO18, GPIO20, GPIO21, GPIO33, GPIO34, GPIO35, GPIO36, GPIO37, GPIO39, GPIO40, GPIO41, GPIO42, GPIO43, GPIO44, GPIO45, GPIO46, GPIO47, GPIO48

**Available Pins:** GPIO0, GPIO3, GPIO16, GPIO19, GPIO24-25, GPIO26-32 (excluding used pins)

**Reserved/Avoid:** GPIO26-32 (SPI Flash/PSRAM), GPIO45-46 (Strapping pins - but GPIO45-48 are used for LED driver)

### 🎯 Pin Selection Rationale

**Display Pins (GPIO2, 4-7, 21):** Grouped together for efficient SPI communication and signal integrity.

**Touch I2C (GPIO9, 20):** GPIO9 is the default I2C SCL pin on ESP32-S3, GPIO20 is default SDA alternative.

**Inter-MCU UART (GPIO17-18):** Uses UART1 with high-speed capable pins for audio data transfer.

**SD Card (GPIO10-13):** Dedicated SPI bus to avoid conflicts with display and inter-MCU communication.

**Design Principles:**
- ✅ No strapping pin conflicts (GPIO0, 3 avoided for critical functions)
- ✅ SPI Flash/PSRAM pins (GPIO26-32) completely avoided
- ✅ Grouped related functions on same SPI buses (Display VSPI, SD Card HSPI, LED Driver HSPI)
- ✅ Dedicated I2C buses (Touch I2C0, Button Matrix I2C1)
- ✅ ADC pin (GPIO1) dedicated to potentiometer multiplexer
- ✅ UART interfaces properly separated (UART1 for inter-MCU, UART2 for MIDI)
- ✅ GPIO45-48 used for LED driver (strapping pins but safe when not booting)

### 🔧 Hardware Validation Status

**✅ Verified Components:**
- ST7796S Display: Full initialization and LVGL integration working
- FT6x36 Touch Controller: I2C communication and touch event handling
- Inter-MCU UART: Design completed, using UART1 on GPIO17/18
- SD Card Interface: Ready for data logging and preset storage

**🆕 New Components Added:**
- CD74HC4067: 16-channel potentiometer multiplexer with 12-bit ADC
- TCA8418: 8x8 capacitive button matrix with I2C interface
- TLC5947: 48-channel 12-bit PWM LED driver with SPI interface
- Enhanced MIDI: Both USB and DIN interfaces supported

**🎯 Target Board:** ESP32-S3-DevKitC-1 (N8R8 PSRAM variant recommended)

**📚 Reference:** Pin assignments verified against [ESP32-S3 DevKitC Official Pinout](https://randomnerdtutorials.com/esp32-s3-devkitc-pinout-guide/)
