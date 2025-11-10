# WaveX - Dual-MCU Sampler/Synth

A high-performance dual-MCU sampler and synthesizer featuring an ESP32-P4 frontend and Daisy Seed (STM32H750) backend.

## Architecture Overview

**WaveX** implements a dual-microcontroller architecture optimizing each MCU for its specific role:

### Backend - Audio Engine (Daisy Seed/STM32H750)
- **Real-time audio processing** at 48kHz/24-bit
- **Sample playback engine** with interpolation and loop support
- **Synthesis modules**: Envelopes, LFOs, filters, modulation matrix
- **CV outputs** via SPI DACs for modular integration
- **High-performance DSP** using ARM Cortex-M7 at 480MHz

### Frontend - User Interface (ESP32-P4)
- **Touchscreen interface** using LVGL graphics library
- **File management** with SD card and USB connectivity
- **MIDI I/O** for external controller integration
- **User controls** and parameter adjustment
- **Sample management** and preset storage

### Inter-MCU Communication
- **High-speed SPI protocol** (10+ Mbps) for control data and sample transfer
- **Custom protocol** optimized for real-time audio applications with packet statistics
- **Shared library** for protocol definitions and utilities
- **Master-slave communication** with comprehensive testing and monitoring
- **Interrupt-driven data transfer** for continuous communication verification
- **Packet statistics tracking** for debugging and performance analysis

## Project Structure

```
WaveX/
├── firmware/
│   ├── esp32/                 # ESP32-P4 Frontend Firmware
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
- **Build System**: Unified Makefile and `idf.py` build successful for ESP32-P4 with unnecessary components disabled
- **ESP32-P4 Firmware**: Project structure, builds successfully with zero warnings
- **Hardware Compatibility**: All GPIO assignments verified for ESP32-P4-WIFI6
- **Display Driver**: MIPI-DSI display support via `esp_lcd` and `esp_lvgl_port`
- **Touch Integration**: Capacitive touch with LVGL integration and interrupt handling
- **UI System**: Complete LVGL-based UI navigation system with pages, menus, and input handling
- **Encoder Support**: PCNT-based quadrature encoder support for parameter control
- **Shared Protocol Library**: SPI communication protocol definitions and implementations
- **Inter-MCU Communication**: Full master-slave SPI protocol with packet statistics and testing
- **Daisy Firmware**: QSPI flash configuration, automated build/flash process
- **Git Submodules**: LVGL, libDaisy, DaisySP properly configured
- **Documentation**: Architecture and setup documentation updated

### 🔄 In Progress
- **Audio Engine**: Basic audio processing framework on Daisy backend
- **Sample Management**: File system integration for sample storage and loading
- **MIDI Integration**: USB and UART MIDI input/output support
- **CV Outputs**: MCP4728 DAC integration for modular synthesizer control

### 📋 Next Steps
- Complete audio engine structure for Daisy with sample playback
- Add sample loading and management functionality
- Implement real-time parameter synchronization between MCUs
- Test hardware integration with complete dual-MCU setup
- Add file system support for sample storage

## Development Setup

### Prerequisites

- **Python 3.10+** for development tools
- **VS Code** with Dev Containers extension (recommended)
- **Git** with submodules support

### Quickstart (Devcontainer - Recommended)

1. **Clone the repo**
   ```bash
   git clone --recursive https://github.com/your-org/wavex.git
   cd wavex
   ```

2. Open in VS Code with the [Dev Containers extension](https://code.visualstudio.com/docs/devcontainers/containers)

3. Hit F1 → "Reopen in Container"

4. **Set up development environment**:
   ```bash
   # Activate virtual environment (created automatically)
   source .env/bin/activate

   # Install pre-commit hooks
   pre-commit install
   ```

5. **Build both systems**: `make all` (builds ESP32 + Daisy with enhanced visual output)

6. **Flash firmware**: `make esp32-flash` and flash Daisy via DFU

7. Follow the [setup guide](./setup.md) for detailed build, flash, and workflow instructions

### Alternative Setup (Local Development)

If you prefer not to use devcontainers:

1. **Install system dependencies**:
   ```bash
   # Ubuntu/Debian
   sudo apt-get update
   sudo apt-get install -y cmake ninja-build git python3 python3-pip clang-format
   ```

2. **Set up Python virtual environment**:
   ```bash
   python3 -m venv .env
   source .env/bin/activate
   pip install pre-commit
   pre-commit install
   ```

3. **Install ESP-IDF and ARM GCC** (see setup.md for details)

4. **Initialize submodules**:
   ```bash
   git submodule update --init --recursive
   ```

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

### Testing Commands

```bash
make test          # Run all unit tests
make test-shared   # Test shared protocol library
make test-esp32    # Test ESP32 components
make test-daisy    # Test Daisy components
make test-clean    # Clean test build artifacts
```

## CI/CD and Code Quality

### GitHub Actions CI

WaveX uses GitHub Actions for continuous integration on all pushes and pull requests to `main` and `develop` branches. The CI pipeline runs pre-commit on all files to ensure:

- **Code formatting** validation (clang-format, black, prettier)
- **Code quality** checks (flake8, trailing whitespace, etc.)
- **Build verification** for both ESP32 and Daisy firmware
- **Unit test execution** for all components (shared protocol, ESP32 UI, Daisy audio engine)
- **Caching build artifacts** for faster subsequent runs

**Note**: CI runs on all files to catch any issues, while local pre-commit hooks only run on changed files for efficiency.

### Pre-commit Hooks

Code quality is enforced through pre-commit hooks that run automatically before each commit:

#### Automatic Code Formatting
- **C/C++**: clang-format with Google style (4-space indent, 100-column limit)
- **Python**: black formatter with isort import sorting
- **YAML**: prettier formatting

#### Quality Checks
- **Trailing whitespace** removal
- **End-of-file** fixes
- **Merge conflict** detection
- **Large file** prevention

#### Build & Test Validation
- **ESP32 build** verification
- **Daisy build** verification
- **Unit test execution** (shared, ESP32, Daisy components)

#### Setup Pre-commit Hooks
```bash
# Activate virtual environment
source .env/bin/activate

# Install pre-commit hooks
pre-commit install

# Run on staged files only (recommended for local development)
pre-commit run

# Run on all files (only for CI or complete repo validation)
pre-commit run --all-files

# Update hooks to latest versions
pre-commit autoupdate
```

#### Pre-commit Configuration

The `.pre-commit-config.yaml` defines all quality checks. Key features:
- **Selective execution**: Hooks only run on relevant file changes
- **Automatic staging**: Pre-commit hooks run only on files staged for commit
- **Caching**: Tools are cached for faster execution
- **CI integration**: Same checks run in GitHub Actions
- **Auto-fixing**: Many issues are automatically resolved

## Hardware Requirements

### ESP32-P4 Frontend
- ESP32-P4-WIFI6 with PSRAM support
- 5" MIPI DSI display (1280x720)
- GT911 capacitive touch controller (I2C)
- Rotary encoder with push button (PCNT)
- Button matrix interface (TCA8418)
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

### ESP32-P4 Frontend Pin Assignments
**⚠️ All pin assignments verified for ESP32-P4-WIFI6 compatibility**

#### 🖥️ MIPI-DSI Display Controller
```
Display Pin     Signal          GPIO    Pin Location    Description
-----------     ------          ----    ------------    -----------
1               VCC             3.3V    Power          Display power positive
2               GND             GND     Power          Display power ground
3               MIPI_D0P        GPIO2   —              MIPI Data Lane 0 Positive
4               MIPI_D0N        GPIO3   —              MIPI Data Lane 0 Negative
5               MIPI_D1P        GPIO4   —              MIPI Data Lane 1 Positive
6               MIPI_D1N        GPIO5   —              MIPI Data Lane 1 Negative
7               MIPI_CLKP       GPIO6   —              MIPI Clock Lane Positive
8               MIPI_CLKN       GPIO7   —              MIPI Clock Lane Negative
9               MIPI_RST        GPIO8   —              Display Reset
10              MIPI_BL         GPIO9   —              Backlight Control
```

#### 👆 I2C Capacitive Touch Panel
```
Touch Pin       Signal          GPIO    Pin Location    Description
---------       ------          ----    ------------    -----------
CTP_SCL         Touch SCL       GPIO21  J3-4           I2C Clock for touch controller
CTP_SDA         Touch SDA       GPIO20  J3-19          I2C Data for touch controller
CTP_INT         Touch INT       GPIO15  J1-21          Interrupt pin
CTP_RST         Touch RST       GPIO14  J1-20          Reset pin
```

**✅ Touch Technology**: Firmware supports capacitive touch controllers with LVGL integration.

**📡 MIPI-DSI Configuration**: High-speed MIPI-DSI interface provides superior display performance compared to SPI, with LVGL integration for professional UI rendering.

#### 🎛️ Rotary Encoders (MCP3008 ADC)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
ADC_SCK         GPIO46  —              SPI Clock for MCP3008
ADC_MOSI        GPIO47  —              SPI MOSI for MCP3008
ADC_MISO        GPIO52  —              SPI MISO for MCP3008
ADC_CS          GPIO29  —              Chip Select for MCP3008
```
**✅ Technology**: MCP3008 8-channel 10-bit ADC for rotary encoders and potentiometers

#### 🔘 Button Matrix Interface (TCA8418)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
TCA8418_INT    GPIO30  —              Interrupt pin (shared I2C bus with touch)
```
**✅ Technology**: 8x8 capacitive touch button matrix with I2C interface (shared with touch controller)

#### 💡 LED Driver Interface (TLC5947)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
TLC5947_LAT     GPIO28  —              Latch pin (XLAT)
TLC5947_BLANK   GPIO27  —              Output enable / BLANK
```
**✅ Technology**: 48-channel 12-bit PWM LED driver with SPI interface (shared SPI2 bus)

#### 🔗 Inter-MCU Communication (ESP32 ↔ Daisy)
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
SPI_SCLK        GPIO48  —              SPI Clock (ESP32 as slave)
SPI_MOSI        GPIO49  —              SPI MOSI from Daisy
SPI_MISO        GPIO50  —              SPI MISO to Daisy
SPI_CS          GPIO51  —              SPI Chip Select from Daisy
ESP_ATTN        GPIO31  J3-14          ESP32 attention output to Daisy
```
**Note**: Uses SPI with ESP32 as slave and Daisy as master for high-speed communication at 4MHz with interrupt-driven data transfer

**🔌 Quick Connection Guide:**
- **ESP32 GPIO48** → **Daisy D8** (SCLK)
- **ESP32 GPIO49** → **Daisy D10** (MOSI)
- **ESP32 GPIO50** → **Daisy D9** (MISO)
- **ESP32 GPIO51** → **Daisy D7** (CS)
- **ESP32 GPIO31** → **Daisy D0** (ATTN)
- **ESP32 GND** → **Daisy GND**

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

#### 🎛️ PCNT Encoder
```
Signal          GPIO    Pin Location    Description
------          ----    ------------    -----------
ENCODER_A       GPIO33  J3-7           Encoder Channel A (PCNT input)
ENCODER_B       GPIO34  J3-8           Encoder Channel B (PCNT input)
ENCODER_BTN     GPIO40  J3-12          Encoder Push Button
```
**✅ Technology**: Quadrature encoder using ESP32-P4 PCNT peripheral for high-precision parameter control

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
SPI_SCK         D8      SPI1_SCK (clock to ESP32 - Daisy as master)
SPI_MOSI        D10     SPI1_MOSI (data to ESP32)
SPI_MISO        D9      SPI1_MISO (data from ESP32)
SPI_CS          D7      SPI1_NSS (chip select to ESP32)
DAISY_IRQ       D13     Push-pull IRQ to ESP32 (data ready)
ESP_ATTN        D14     Attention input from ESP32 (wakeup)
GND             GND     Common ground
```
**Note**: Uses SPI1 with Daisy as master and ESP32 as slave for high-speed communication at 10MHz with interrupt-driven data transfer and comprehensive packet statistics

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
- **GPIO17-18**: Available for additional functions (was inter-MCU UART, now using SPI3)

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

#### 🔧 Temporary MVP: SPI SD Card on Daisy (wiring + migration notes)

For the initial MVP (before migrating to SDIO), the Daisy backend uses a simple SPI-based microSD connection. This is isolated to `src/storage/` so we can swap to SDIO later without touching higher-level code.

Temporary SPI3 wiring (STM32H7 hardware SPI3):

```
Signal   Daisy Pin   STM32 Pin   Notes
------   ---------   ---------   ------------------------------
SCK      D2          PC10        SPI3_SCK
MISO     D1          PC11        SPI3_MISO
MOSI     D6          PC12        SPI3_MOSI
CS       D9          PB4         GPIO (manual CS)
VCC      3V3                     3.3 V only
GND      GND                     Common ground
```

Implementation details:
- SPI stack: `SpiHandle` (libDaisy) → minimal SD-over-SPI block I/O → FatFs `diskio` glue.
- Files added under Daisy firmware:
  - `src/storage/sd_spi.{h,cpp}`: low-level SPI SD init + single-sector read/write
  - `src/storage/diskio_sd_spi.cpp`: FatFs `diskio` hooks for the SPI drive
  - `src/storage/fs_browse.{h,cpp}`: directory listing helper for UI/browse protocol
- On boot we mount FatFs (`f_mount`) and log success/fail.

Performance and guardrails:
- Init at ~400 kHz, then bump to ~20–24 MHz after `ACMD41/CMD58`.
- Uses blocking SPI for simplicity and cache safety; DMA can be added later.
- 512B-aligned sector transfers; no logging inside audio callback.

Planned migration to SDIO (native MMC/SD):
- The FatFs surface (`diskio_*` functions) is isolated. To migrate:
  1. Implement a new backend (e.g., `diskio_sd_sdio.cpp`) that uses libDaisy SDMMC or a dedicated SDIO driver.
  2. Keep `fs_browse` and all higher-level code unchanged.
  3. Switch the drive registration in the build to the SDIO backend.
- Pin changes: SDIO uses dedicated pins on Daisy (D2–D6 free up; refer to Daisy SDMMC example under `libs/libDaisy/examples/SDMMC_HelloWorld`).

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
│    ESP32-P4     │◄───  UART Bus  ────►│  Daisy Seed     │
│   (Frontend)    │                    │   (Backend)     │
│                 │                    │                 │
│ ┌─────────────┐ │                    │ ┌─────────────┐ │
│ │ MIPI DSI    │ │                    │ │   AK4556    │ │
│ │ 1280x720    │ │                    │ │   Codec     │ │
│ │             │ │                    │ │ 48kHz/24bit │ │
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

#### ESP32-P4 Frontend
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
Protocol        Clock       Max Data Rate    Latency    Features
--------        -----       -------------    -------    --------
Inter-MCU SPI   10MHz       10+ Mbps        <1ms       DMA support, interrupt-driven, packet stats
Audio Callback  48kHz       64 samples       1.33ms     Real-time audio processing
Parameter Sync  1kHz        Control rate     1ms        MIDI and control updates
MIPI-DSI        Variable    High bandwidth   <16ms      LVGL rendering, hardware acceleration
```

### Development/Debug Interfaces

#### ESP32-P4 Debug
- **USB-Serial**: Built-in USB-to-Serial for programming/debug
- **JTAG**: Standard 20-pin JTAG for hardware debugging

#### Daisy Seed Debug
- **USB-DFU**: Built-in DFU bootloader for programming
- **SWD**: 4-pin SWD interface for debugging
- **UART**: Serial debug output via USB

## Hardware Pin Documentation

Based on the provided LCD/touch controller specs, here are the key signals and their behaviors, mapped to ESP32-P4 pins (from `pin_config.h`):

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
| CTP_SDA | I2C data | N/A | GPIO23 | Touch data signal |
| CTP_INT | Touch interrupt | Low on touch | GPIO15 | Falling-edge interrupt; enable pull-up |
| CTP_RST | Touch reset | Low reset | GPIO14 | Assert low to reset |

**Troubleshooting Tips**:
- **GPIO Stability**: If pin levels don't hold (e.g., reset reads 0 after set to 1), enable internal pull-ups in `ui_main.cpp` GPIO config. For stronger pull, add external 4.7kΩ resistors to 3.3V.
- **Interrupt Mode**: Touch uses interrupt-driven polling for efficiency; verify CTP_INT pulls low on touch with multimeter.
- **PWM Backlight**: Brightness controlled via LEDC (5kHz); test with duty cycle 128 (~50% brightness).
- **Verification**: After init, check logs for pin states (e.g., "Reset pin state: 1" for deasserted).

For full pinout and wiring diagram, see the "Hardware Pinout & Wiring" section below.

## Configuration Flags & Debug Options

WaveX firmware includes several compile-time configuration flags for debugging, testing, and performance tuning.

### Daisy Backend Configuration (`firmware/daisy/src/config.hpp`)

#### UART Communication Control
```cpp
// Daisy <-> ESP32 UART baud rate (default: 460800)
#define INTER_MCU_UART_BAUD_RATE 460800

// Completely disable UART init and polling for isolation testing (default: 0)
#define WAVEX_DEBUG_DISABLE_UART 0

// Enable UART RX via IRQ vs polling mode (default: 1 = IRQ mode)
#define WAVEX_UART_RX_IRQ_MODE 1

// Enable verbose UART debug logging - TX/RX messages, sync, heartbeats (default: 0)
#define WAVEX_UART_DEBUG_LOG 0
```

**Usage Examples:**
- **Silent UART**: Set `WAVEX_UART_DEBUG_LOG = 0` (default) for production builds
- **Verbose Debug**: Set `WAVEX_UART_DEBUG_LOG = 1` to see all UART packet details
- **UART Isolation**: Set `WAVEX_DEBUG_DISABLE_UART = 1` to disable inter-MCU communication
- **Polling Mode**: Set `WAVEX_UART_RX_IRQ_MODE = 0` for simpler but less efficient reception

#### Build Configuration
```cpp
// Daisy storage destination (default: qspi for 7.75MB flash)
#define DAISY_STORAGE qspi

// Use QSPI flash placement (default: ON)
#define DAISY_USE_QSPI_FLASH ON

// Build for bootloader QSPI (default: BOOT_QSPI)
#define APP_TYPE BOOT_QSPI
```

### ESP32 Frontend Configuration

#### Display & Touch
```cpp
// Enable/disable display initialization (default: enabled)
#define WAVEX_DISPLAY_ENABLED 1

// Enable/disable touch input (default: enabled)
#define WAVEX_TOUCH_ENABLED 1

// Display brightness (0-255, default: 128)
#define WAVEX_DISPLAY_BRIGHTNESS 128
```

#### Development & Debug
```cpp
// Enable verbose logging (default: 0)
#define WAVEX_VERBOSE_LOGGING 0

// Enable performance profiling (default: 0)
#define WAVEX_PERFORMANCE_PROFILING 0

// Enable memory usage tracking (default: 0)
#define WAVEX_MEMORY_TRACKING 0
```

### Runtime Configuration

#### Audio Settings
```cpp
// Audio block size (default: 48 samples = 1ms at 48kHz)
#define WAVEX_AUDIO_BLOCK_SIZE 48

// Audio sample rate (default: 48kHz)
#define WAVEX_AUDIO_SAMPLE_RATE 48000

// Enable/disable audio processing (default: enabled)
#define WAVEX_AUDIO_ENABLED 1
```

#### Storage & File System
```cpp
// SD card timeout in milliseconds (default: 1000ms)
#define WAVEX_SD_TIMEOUT_MS 1000

// Maximum file path length (default: 96 characters)
#define WAVEX_MAX_PATH_LENGTH 96

// Enable/disable file system logging (default: 0)
#define WAVEX_FS_DEBUG_LOG 0
```

### How to Modify Configuration

1. **Edit the config file**: Modify `firmware/daisy/src/config.hpp` for Daisy, or create similar for ESP32
2. **Rebuild**: Run `make daisy` or `make esp32` to apply changes
3. **Flash**: Upload the new firmware to test configuration changes

### Recommended Production Settings
```cpp
// Daisy production config
#define WAVEX_DEBUG_DISABLE_UART 0      // Keep UART enabled
#define WAVEX_UART_DEBUG_LOG 0          // Disable verbose logging
#define WAVEX_UART_RX_IRQ_MODE 1        // Use efficient IRQ mode

// ESP32 production config
#define WAVEX_VERBOSE_LOGGING 0         // Disable debug output
#define WAVEX_PERFORMANCE_PROFILING 0   // Disable profiling overhead
#define WAVEX_MEMORY_TRACKING 0         // Disable memory tracking
```

### Debug vs Production Builds

**Debug Build** (development/testing):
- Enable verbose logging: `WAVEX_UART_DEBUG_LOG = 1`
- Enable performance profiling: `WAVEX_PERFORMANCE_PROFILING = 1`
- Enable memory tracking: `WAVEX_MEMORY_TRACKING = 1`

**Production Build** (deployment):
- Disable all debug features: set flags to 0
- Keep essential functionality: UART communication, audio processing
- Optimize for performance and minimal memory usage

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

## ESP32-P4 Pin Assignments

WaveX uses an ESP32-P4-WIFI6 as the frontend MCU with the following comprehensive pin assignments:

### 🖥️ MIPI-DSI Display Controller
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| MIPI D0P | GPIO2 | MIPI-DSI | ✅ Verified |
| MIPI D0N | GPIO3 | MIPI-DSI | ✅ Verified |
| MIPI D1P | GPIO4 | MIPI-DSI | ✅ Verified |
| MIPI D1N | GPIO5 | MIPI-DSI | ✅ Verified |
| MIPI CLKP | GPIO6 | MIPI-DSI | ✅ Verified |
| MIPI CLKN | GPIO7 | MIPI-DSI | ✅ Verified |
| MIPI RST | GPIO8 | MIPI-DSI | ✅ Verified |
| MIPI BL | GPIO9 | MIPI-DSI | ✅ Verified |

### 👆 Touch Controller (Capacitive)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| Touch Data (SDA) | GPIO20 | J3-19 | ✅ Verified |
| Touch Clock (SCL) | GPIO21 | J3-4 | ✅ I2C SCL |
| Reset (RST) | GPIO14 | J1-20 | ✅ Verified |
| Interrupt (INT) | GPIO15 | J1-21 | ✅ Verified |

### 🔗 Inter-MCU Communication (ESP32 ↔ Daisy)
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| SPI SCLK | GPIO48 | ESP32-P4 GPIO48 | ✅ Verified |
| SPI MOSI | GPIO49 | ESP32-P4 GPIO49 | ✅ Verified |
| SPI MISO | GPIO50 | ESP32-P4 GPIO50 | ✅ Verified |
| SPI CS | GPIO51 | ESP32-P4 GPIO51 | ✅ Verified |
| ESP ATTN | GPIO31 | J3-14 | ✅ Verified |
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
| Address A2 | GPIO35 | J3-9 | ❌ Reassigned (use GPIO41 for ESP_ATTN) |
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

### 🎛️ Single PCNT Encoder - CRITICAL COMPONENT #4
| Function | GPIO | Pin Location | Status |
|----------|------|--------------|--------|
| Channel A | GPIO33 | J3-7 | ✅ Verified |
| Channel B | GPIO34 | J3-8 | ✅ Verified |
| Push Button | GPIO40 | J3-12 | ✅ Verified |

### 📌 Pin Usage Summary

**MIPI-DSI Pins:** GPIO2-9 (MIPI-DSI interface)
**Inter-MCU SPI Pins:** GPIO48, GPIO49, GPIO50, GPIO51 (SPI communication)
**Touch Pins:** GPIO14, GPIO15, GPIO20, GPIO21 (Capacitive touch)
**Encoder Pins:** GPIO33, GPIO34, GPIO40 (PCNT encoder)
**Available Pins:** GPIO1, GPIO16-GPIO19, GPIO22-GPIO30, GPIO32, GPIO35-GPIO39, GPIO41-GPIO47, GPIO52 (excluding used and reserved pins)

**Reserved/Avoid:** GPIO0 (strapping), GPIO26-32 (SPI Flash/PSRAM)

### 🎯 Pin Selection Rationale

**MIPI-DSI Pins (GPIO2-9):** Dedicated MIPI-DSI interface pins for high-speed display communication with hardware acceleration.

**Touch I2C (GPIO20, 21, 14, 15):** Uses available GPIO pins for capacitive touch controller communication with interrupt support.

**Inter-MCU SPI (GPIO48-51):** Uses dedicated high-numbered GPIO pins for high-speed Daisy communication with interrupt-driven data transfer.

**Encoder (GPIO33, 34, 40):** Uses available GPIO pins with PCNT peripheral for high-precision quadrature decoding.

**Design Principles:**
- ✅ No strapping pin conflicts (GPIO0, GPIO45-48 avoided)
- ✅ MIPI-DSI interface uses dedicated pins (GPIO2-9) for optimal performance
- ✅ SPI Flash/PSRAM pins (GPIO26-32) preserved for system use
- ✅ ESP32-P4 available GPIO pins (GPIO1, GPIO16-19, GPIO22-30, GPIO32, GPIO35-39, GPIO41-47, GPIO52) for future expansion
- ✅ Dedicated SPI bus for inter-MCU communication
- ✅ Dedicated I2C bus for touch controller
- ✅ PCNT peripheral for encoder (GPIO33-34)

### 🔧 Hardware Validation Status

**✅ Verified Components:**
- MIPI-DSI Display: Full initialization and LVGL integration working
- Capacitive Touch Controller: I2C communication and touch event handling with interrupts
- UI Navigation System: Complete page-based navigation with menus and input handling
- Inter-MCU SPI: Full master-slave protocol with packet statistics and testing
- PCNT Encoder: High-precision quadrature encoder support for parameter control
- UART Communication: Inter-MCU UART link for control data

**🆕 New Components Added:**
- PCNT Encoder: High-precision quadrature encoder input using ESP32-P4 PCNT peripheral
- UI Navigation System: Complete LVGL-based navigation with pages, menus, and softkeys
- Button Matrix Support: TCA8418 capacitive button matrix interface

**🔧 Pin Conflicts Resolved:**
- MIPI-DSI interface uses dedicated pins (GPIO2-9) for optimal performance
- ESP32-P4 available GPIO pins preserved for future expansion
- Strapping pins (GPIO0, GPIO45-48) completely avoided
- SPI Flash/PSRAM pins (GPIO26-32) preserved for system use
- All inter-MCU SPI pins use dedicated GPIO pins for reliable communication

**🎯 Target Board:** ESP32-P4-WIFI6 with PSRAM support

**📚 Reference:** Pin assignments verified against ESP32-P4-WIFI6 datasheet and hardware specifications

**🚀 Future Expansion Ready:** With MIPI-DSI using dedicated pins and efficient pin allocation, there's plenty of room for additional peripherals and future expansion.
