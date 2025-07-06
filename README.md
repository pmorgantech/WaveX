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
- **Development Environment**: Devcontainer with ESP-IDF 5.2 and ARM GCC toolchain
- **ESP32 Firmware**: Basic project structure, builds successfully
- **Shared Protocol Library**: SPI communication protocol definitions
- **Build System**: Makefile and scripts for both firmwares
- **Git Submodules**: LVGL, libDaisy, DaisySP properly configured
- **Documentation**: Architecture and setup documentation

### 🔄 In Progress
- **Daisy Firmware**: Build issues with HAL dependencies (known libDaisy version compatibility issue)
- **Protocol Implementation**: Complete SPI communication handlers
- **LVGL Integration**: ESP32 display and touch drivers

### 📋 Next Steps
- Resolve libDaisy compilation issues or use alternative STM32 framework
- Implement LVGL display and touchscreen drivers for ESP32
- Create basic audio engine structure for Daisy
- Add sample loading and playback functionality
- Implement real-time parameter communication between MCUs

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
- XPT2046 touch controller
- SD card interface
- USB MIDI interface
- SPI connection to Daisy

### Daisy Seed Backend
- Daisy Seed (STM32H750VBT6)
- Audio I/O connections
- SPI DAC for CV outputs
- SPI connection to ESP32

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
