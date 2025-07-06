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

## Build Instructions

### Prerequisites
- Docker installed on your system
- Git with submodule support

### Quick Start

1. **Clone the repository:**
```bash
git clone --recursive https://github.com/yourusername/WaveX.git
cd WaveX
```

2. **Build the devcontainer:**
```bash
docker build -t wavex-dev .devcontainer/
```

3. **Build ESP32 firmware:**
```bash
# Build using ESP-IDF container
docker run --rm -v $(pwd):/workspace espressif/idf:release-v5.2 \
  /bin/bash -c "cd /workspace/firmware/esp32 && idf.py build"

# Or use the Makefile
make esp32
```

4. **Build Daisy firmware (when fixed):**
```bash
# Using custom devcontainer with ARM toolchain
docker run --rm -v $(pwd):/workspace wavex-dev \
  /bin/bash -c "cd /workspace/firmware/daisy && cmake -B build && cd build && make"

# Or use the Makefile
make daisy
```

### Development Container

The project includes a pre-configured development container with:
- ESP-IDF 5.2 for ESP32 development
- ARM GCC toolchain for STM32 development
- CMake, Ninja, and other build tools
- Debug tools (OpenOCD, GDB, etc.)

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
