# WaveX Daisy Firmware Build Wrapper
# This Makefile configures CMake properly and delegates to the build directory

CMAKE_SOURCE_DIR := $(CURDIR)
# Overridable so CI can build a second flag-set configuration (roadmap Phase
# 1 item 1, e.g. BUILD_DIR=build-stageb) into its own directory without
# disturbing the default build/.
BUILD_DIR ?= build
# Extra -D args passed through to the CMake configure step, e.g.
# CMAKE_EXTRA_ARGS="-DWAVEX_VOICE_OUTPUT_BACKEND=1 -DWAVEX_CV_BACKEND=1 -DWAVEX_ANALOG_CV_GROUPS=8"
CMAKE_EXTRA_ARGS ?=
CMAKE := cmake
MAKE := make
TOOLCHAIN_FILE := $(CMAKE_SOURCE_DIR)/libs/libDaisy/cmake/toolchains/ArmGNUToolchain.cmake
COMPILER_C := /usr/bin/arm-none-eabi-gcc
COMPILER_CXX := /usr/bin/arm-none-eabi-g++
TOOLCHAIN_ENV := CMAKE_C_COMPILER=$(COMPILER_C) CMAKE_CXX_COMPILER=$(COMPILER_CXX)
OBJCOPY := /usr/bin/arm-none-eabi-objcopy
BIN_FILE := $(BUILD_DIR)/wavex-daisy.bin

.PHONY: all build clean help configure
.PHONY: flash dfu sd usb bin run auto-flash

# Default target: build the firmware
all: build

# Configure CMake if build directory doesn't exist or CMakeCache is missing
configure:
	@if [ ! -d "$(BUILD_DIR)" ] || [ ! -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		echo "Configuring CMake..."; \
		mkdir -p $(BUILD_DIR); \
		cd $(BUILD_DIR) && $(TOOLCHAIN_ENV) $(CMAKE) $(CMAKE_SOURCE_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) $(CMAKE_EXTRA_ARGS); \
	fi

# Build the firmware
build: configure
	@echo "Building firmware..."
	@cd $(BUILD_DIR) && $(MAKE)

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f *.elf *.hex *.bin
	@echo "Clean complete"

# Build and generate binary file
bin: build
	@$(OBJCOPY) -O binary -S $(BUILD_DIR)/wavex-daisy.elf $(BIN_FILE)
	@cp $(BIN_FILE) /tmp/wavex-daisy.bin
	@echo "Binary files generated in $(BUILD_DIR)/"

# Build and flash via DFU (automated)
flash: bin
	@echo "Flashing firmware via DFU..."
	@if [ ! -f "$(BUILD_DIR)/wavex-daisy.bin" ]; then \
		echo "Error: Binary file not found. Build failed?"; \
		exit 1; \
	fi
	@echo "Put Daisy into DFU mode (hold BOOT while powering on) just before this step finishes."
	@if ! command -v dfu-util >/dev/null 2>&1; then \
		echo "Error: dfu-util not found. Install with: sudo apt install dfu-util"; \
		exit 1; \
	fi
	@dfu-util -d 0483:df11 -s 0x90040000:leave -D $(BUILD_DIR)/wavex-daisy.bin
	@echo "DFU flash completed (if the device accepted the transfer)."
	@echo "Binary location: $(BUILD_DIR)/wavex-daisy.bin"

# Alias for flash
dfu: flash

# Build and prepare for SD card flashing
sd: bin
	@if [ ! -f "$(BUILD_DIR)/wavex-daisy.bin" ]; then \
		echo "Error: Binary file not found. Build failed?"; \
		exit 1; \
	fi
	@cp $(BUILD_DIR)/wavex-daisy.bin /tmp/wavex-daisy.bin
	@echo "Binary copied to /tmp/wavex-daisy.bin"
	@echo "Copy this file to the root of your SD card and insert into Daisy"

# Build and prepare for USB drive flashing
usb: bin
	@if [ ! -f "$(BUILD_DIR)/wavex-daisy.bin" ]; then \
		echo "Error: Binary file not found. Build failed?"; \
		exit 1; \
	fi
	@cp $(BUILD_DIR)/wavex-daisy.bin /tmp/wavex-daisy.bin
	@echo "Binary copied to /tmp/wavex-daisy.bin"
	@echo "Copy this file to USB drive root and connect to Daisy external USB pins"

# Build, flash, and run
run: flash
	@echo "Firmware flashed. Daisy should restart automatically."

# Automated flash (requires openocd and dfu-util)
auto-flash: bin
	@echo "Automated flash (requires openocd and dfu-util)..."
	@if ! command -v openocd >/dev/null 2>&1; then \
		echo "Error: openocd not found. Install with: sudo apt install openocd"; \
		exit 1; \
	fi
	@if ! command -v dfu-util >/dev/null 2>&1; then \
		echo "Error: dfu-util not found. Install with: sudo apt install dfu-util"; \
		exit 1; \
	fi
	@if [ ! -f "$(BUILD_DIR)/wavex-daisy.bin" ]; then \
		echo "Error: Binary file not found. Build failed?"; \
		exit 1; \
	fi
	@echo "Forcing bootloader mode..."
	@openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c "init; reset halt; stm32h7x option_write 0 0x1FF00000 0x1FF00000; reset; shutdown" || true
	@sleep 2
	@echo "Flashing to QSPI..."
	@dfu-util -a 1 -s 0x90040000:leave -D $(BUILD_DIR)/wavex-daisy.bin
	@echo "Restoring normal boot..."
	@openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c "init; reset halt; stm32h7x option_write 0 0x1FF00000 0x08000000; reset; shutdown" || true
	@echo "Flash complete!"

# Show help
help:
	@echo "WaveX Daisy Firmware Build System"
	@echo "=================================="
	@echo ""
	@echo "Available targets:"
	@echo "  make          - Build the firmware (default)"
	@echo "  make build    - Build the firmware"
	@echo "  make clean    - Clean build artifacts"
	@echo "  make bin      - Build and generate binary file"
	@echo "  make flash    - Build and flash via DFU (manual instructions)"
	@echo "  make dfu      - Alias for flash"
	@echo "  make auto-flash - Automated flash (requires openocd and dfu-util)"
	@echo "  make sd       - Build and prepare for SD card flashing"
	@echo "  make usb      - Build and prepare for USB drive flashing"
	@echo "  make run      - Build, flash, and start the Daisy"
	@echo "  make help     - Show this help message"
	@echo ""
	@echo "Build output: $(BUILD_DIR)/wavex-daisy.{elf,hex,bin}"

