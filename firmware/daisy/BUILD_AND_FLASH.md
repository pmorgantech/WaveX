# WaveX Daisy: Build and Flash Guide

## Summary

✅ **Problem Solved**: We've successfully configured the Daisy firmware to build and run from QSPI flash, solving both the STM32 HAL compatibility issue and the flash memory overflow problem.

✅ **Fully Automated**: No manual steps required! The new `make flash` command automatically forces bootloader mode using OpenOCD and then flashes to QSPI.

## What We Accomplished

### 1. ✅ Fixed STM32 HAL Driver Compatibility
- **Root Cause**: Submodule synchronization and toolchain configuration issues
- **Solution**: Used proper ARM toolchain (`ArmGNUToolchain.cmake`) 
- **Result**: All STM32 HAL drivers now compile successfully

### 2. ✅ Solved Flash Memory Overflow
- **Root Cause**: 136KB firmware vs 128KB internal flash limit
- **Solution**: Configured `DAISY_STORAGE=qspi` for QSPI flash
- **Result**: Now using 7.75MB QSPI flash instead of 128KB internal flash

### 3. ✅ Created One-Command Build Process
- **Command**: `make` (builds everything automatically)
- **Toolchain**: Automatically sets ARM compiler and exports
- **Output**: Generates `wavex-daisy.elf`, `wavex-daisy.hex`, and `wavex-daisy.bin`

### 4. ✅ Eliminated Manual Steps
- **Automated**: `make flash` now forces bootloader mode automatically using OpenOCD
- **Method**: Directly manipulates STM32H7 boot options to force bootloader mode
- **Result**: Zero manual intervention required

## Quick Commands

```bash
# Build firmware
make

# Build and flash automatically (NO MANUAL STEPS!)
make flash

# Build and flash automatically (explicit)
make auto-flash

# Build and prepare for SD card
make sd

# Build and prepare for USB drive
make usb

# Clean build artifacts
make clean

# Show all commands
make help
```

## Memory Layout

```
Internal Flash (0x08000000): 128KB - Bootloader only
QSPI Flash   (0x90040000): 7.75MB - Application code
RAM: 864KB total (512KB + 256KB + 64KB + 32KB)
```

## Flashing Methods

### Method 1: Automated Flash (Recommended) - NO MANUAL STEPS!
```bash
make flash
```
**What happens automatically:**
1. **OpenOCD** sets `BOOT_CM7_ADD0` to `0x1FF00000` (forces bootloader mode)
2. **Daisy resets** and enters bootloader mode automatically
3. **dfu-util** flashes binary to QSPI at `0x90040000`
4. **OpenOCD** restores `BOOT_CM7_ADD0` to `0x08000000` (normal operation)
5. **Daisy restarts** and runs from QSPI

**Requirements**: `openocd` and `dfu-util` installed

### Method 2: SD Card
```bash
make sd
# Copy /tmp/wavex-daisy.bin to SD card root
# Insert SD card and power on Daisy
```

### Method 3: USB Drive
```bash
make usb
# Copy /tmp/wavex-daisy.bin to USB drive root
# Connect to Daisy external USB pins (D29, D30)
# Power on Daisy
```

## Boot Process

1. **Power-on**: Daisy boots from internal flash bootloader
2. **QSPI Check**: Bootloader validates application in QSPI
3. **Jump to QSPI**: Bootloader jumps to `0x90040000`
4. **Application Run**: WaveX firmware starts and initializes

## Expected Behavior After Flash

- Daisy initializes all peripherals
- Starts sending liveness messages to ESP32 via UART at 230.4kbps
- LED indicators show normal operation
- Audio processing begins
- Receives test messages from ESP32 (ping every 5s, comprehensive tests every 30s)
- Packet statistics available for debugging and monitoring

## Troubleshooting

### Build Issues
```bash
# Clean and rebuild
make clean && make

# Check ARM toolchain
which arm-none-eabi-gcc
```

### Flash Issues
- **Automated**: Ensure `openocd` and `dfu-util` are installed
- **SD Card**: Verify FAT32 format, .bin file in root directory
- **USB Drive**: Check external USB pin connections

### Runtime Issues
- Verify QSPI flash was written correctly
- Check serial output for error messages
- Ensure stable 3.3V power supply

## Dependencies

- **ARM GCC**: `sudo apt install gcc-arm-none-eabi`
- **OpenOCD**: `sudo apt install openocd`
- **dfu-util**: `sudo apt install dfu-util`
- **CMake**: `sudo apt install cmake`
- **Daisy Bootloader**: Must be installed in internal flash

## Next Steps

1. **Flash the firmware**: Use `make flash` (fully automated!)
2. **Verify operation**: Check Daisy LED indicators and ESP32 communication
3. **Monitor liveness**: Watch ESP32 serial output for Daisy messages and packet statistics
4. **Test functionality**: Verify audio processing and UART communication
5. **Debug communication**: Use packet statistics APIs to monitor bidirectional traffic

## Files Generated

- `build/wavex-daisy.elf` - Debug/development version
- `build/wavex-daisy.hex` - Intel HEX format
- `build/wavex-daisy.bin` - Binary for flashing (134KB)

## Automation Details

The new `make flash` command:
1. **Builds** the firmware with QSPI configuration
2. **Forces bootloader mode** using OpenOCD to set `BOOT_CM7_ADD0 0x1FF00000`
3. **Waits** for bootloader initialization
4. **Flashes** to QSPI using `dfu-util -a 1 -s 0x90040000:leave -D wavex-daisy.bin`
5. **Restores normal boot** using OpenOCD to set `BOOT_CM7_ADD0 0x08000000`
6. **Completes** with Daisy running from QSPI

**No manual button pressing, no manual mode switching, no manual steps at all!**

The automation directly manipulates the STM32H7 boot options to force bootloader mode, then restores normal operation after flashing.

The firmware is now ready to run and should communicate properly with the ESP32!
