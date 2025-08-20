# WaveX Daisy Firmware

This is the Daisy firmware for the WaveX project, configured to run from QSPI flash for maximum memory capacity.

## Quick Start

### Build the firmware
```bash
make
```

### Build and flash to Daisy (via DFU)
```bash
make flash
```

### Build, flash, and run
```bash
make run
```

## Build Commands

| Command | Description |
|---------|-------------|
| `make` | Build the firmware (default) |
| `make build` | Build the firmware |
| `make bin` | Build and generate binary file |
| `make clean` | Clean build artifacts |
| `make dfu` | Build and flash via DFU (USB) |
| `make sd` | Build and prepare for SD card flashing |
| `make usb` | Build and prepare for USB drive flashing |
| `make flash` | Build and flash via DFU (default) |
| `make run` | Build, flash, and start the Daisy |
| `make help` | Show all available commands |

## Memory Configuration

The firmware is configured to use **QSPI flash** instead of internal flash:

- **Internal Flash**: 128KB (0x08000000) - Used for bootloader
- **QSPI Flash**: 7.75MB (0x90040000) - Used for application code
- **RAM**: 512KB + 256KB + 64KB + 32KB = 864KB total

This configuration allows the firmware to be much larger than the 128KB internal flash limit.

## Flashing Process

### Prerequisites
- **Daisy bootloader** must be installed in internal flash
- **dfu-util** tool installed (`sudo apt install dfu-util`)
- Daisy connected via USB and in bootloader mode

### Method 1: DFU (USB) - Recommended
```bash
make flash
```

**Steps:**
1. Hold BOOT button while powering on Daisy
2. LED should pulse sinusoidally (bootloader active)
3. Run `make flash` - this will flash via DFU
4. Daisy will automatically restart and run the new firmware

### Method 2: SD Card
```bash
make sd
```

**Steps:**
1. Run `make sd` to generate binary
2. Copy `/tmp/wavex-daisy.bin` to SD card root directory
3. Insert SD card into Daisy
4. Power on Daisy - bootloader auto-flashes the binary

### Method 3: USB Drive
```bash
make usb
```

**Steps:**
1. Run `make usb` to generate binary
2. Copy `/tmp/wavex-daisy.bin` to USB drive root directory
3. Connect USB drive to Daisy external USB pins (D29, D30)
4. Power on Daisy - bootloader auto-flashes the binary

## QSPI Configuration Details

The firmware uses the `STM32H750IB_qspi.lds` linker script which:

- Places all code sections in QSPI flash at `0x90040000`
- Keeps internal flash free for bootloader
- Provides 7.75MB of program storage
- Maintains proper memory layout for STM32H750

## Boot Process

1. **Power-on**: Daisy boots from internal flash bootloader
2. **QSPI Check**: Bootloader checks for valid application in QSPI
3. **Jump to QSPI**: If valid, bootloader jumps to `0x90040000`
4. **Application Run**: WaveX firmware starts and initializes

## Troubleshooting

### Build Issues
- Ensure ARM toolchain is installed: `sudo apt install gcc-arm-none-eabi`
- Clean and rebuild: `make clean && make`

### Flash Issues
- **DFU**: Ensure Daisy is in bootloader mode (LED pulsing sinusoidally)
- **SD Card**: Verify SD card is properly formatted (FAT32) and has .bin file in root
- **USB Drive**: Check external USB pin connections (D29, D30)
- Verify `dfu-util` is installed: `sudo apt install dfu-util`

### Runtime Issues
- Verify QSPI flash was written correctly
- Check serial output for error messages
- Ensure proper power supply (Daisy needs stable 3.3V)

## Expected Behavior

After successful flash and boot:
- Daisy should initialize all peripherals
- Start sending liveness messages to ESP32 via UART
- LED indicators should show normal operation
- Audio processing should begin
- Begin receiving test messages from ESP32 (visible in packet statistics)

## Communication with ESP32

The Daisy communicates with the ESP32 via UART protocol at 230.4kbps:
- **MSG_METER_PUSH (0x10)**: Audio level data every ~20ms (throttled logging every 100th packet)
- **MSG_WAVE_CHUNK (0x11)**: Waveform preview data when requested
- **MSG_HEARTBEAT (0x12)**: Health status and system metrics
- **Other messages**: As defined in the shared protocol
- **Packet Statistics**: Comprehensive tracking and debugging support
- **Automatic Testing**: ESP32 sends test messages every 5-30 seconds for verification

## Development

### Adding Features
- Modify `src/main.cpp` for main application logic
- Add new source files to `CMakeLists.txt`
- Rebuild with `make`

### Debugging
- Use ST-Link debugger for real-time debugging
- Serial output available via USB CDC
- LED indicators for status information

## Dependencies

- **libDaisy**: Core Daisy framework and STM32 HAL drivers
- **DaisySP**: Audio processing library
- **ARM GCC**: Cross-compilation toolchain
- **CMake**: Build system
- **dfu-util**: DFU flashing tool
- **Daisy Bootloader**: Required for QSPI operation
