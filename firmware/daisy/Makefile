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
# Shared USB board resolver - nothing here should name a /dev/ttyACM number,
# since enumeration order shifts every time a board re-enumerates.
SERIAL_PORTS := $(CMAKE_SOURCE_DIR)/../../scripts/serial_ports.py
# How long `flash` waits for you to hold BOOT and tap RESET.
DFU_WAIT ?= 20

.PHONY: all build clean help configure stale-cache-check
.PHONY: flash dfu sd usb bin run auto-flash flash-auto

# Default target: build the firmware
all: build

# A CMake build tree bakes in absolute paths, so one configured in the
# devcontainer (/workspaces/WaveX, cmake under /opt/esp) breaks when reused on
# the host (/home/<user>/..., /usr/bin/cmake) and vice versa - the failure is
# an opaque "No such file or directory" from a generated makefile. Detect the
# mismatch and reconfigure instead of making the user run `make clean`.
stale-cache-check:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		cached_src=$$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' $(BUILD_DIR)/CMakeCache.txt); \
		cached_cmake=$$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' $(BUILD_DIR)/CMakeCache.txt); \
		if [ "$$cached_src" != "$(CMAKE_SOURCE_DIR)" ] || [ ! -x "$$cached_cmake" ]; then \
			echo "Build dir was configured for a different environment:"; \
			echo "  source: $$cached_src (now $(CMAKE_SOURCE_DIR))"; \
			echo "  cmake:  $$cached_cmake"; \
			echo "Reconfiguring $(BUILD_DIR) from scratch..."; \
			rm -rf $(BUILD_DIR); \
		fi; \
	fi

# Configure CMake if build directory doesn't exist or CMakeCache is missing
configure: stale-cache-check
	@if [ ! -d "$(BUILD_DIR)" ] || [ ! -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		echo "Configuring CMake..."; \
		mkdir -p $(BUILD_DIR); \
		cd $(BUILD_DIR) && $(TOOLCHAIN_ENV) $(CMAKE) $(CMAKE_SOURCE_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) $(CMAKE_EXTRA_ARGS); \
	fi

# Build the firmware. Parallel by default; override with e.g. JOBS=1 to get
# readable serial output when chasing a compile error.
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
build: configure
	@echo "Building firmware (-j$(JOBS))..."
	@cd $(BUILD_DIR) && $(MAKE) -j$(JOBS)

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
	@if ! command -v dfu-util >/dev/null 2>&1; then \
		echo "Error: dfu-util not found. Install with: sudo apt install dfu-util"; \
		exit 1; \
	fi
	@echo "Waiting up to $(DFU_WAIT)s for the Daisy in DFU mode (hold BOOT, tap RESET)..."
	@if ! python3 $(SERIAL_PORTS) --present daisy-dfu --wait $(DFU_WAIT); then \
		echo "Daisy never entered DFU. Hold BOOT, tap RESET, then re-run."; \
		echo "If the firmware is alive, \`make flash-auto\` needs no buttons."; \
		exit 1; \
	fi
	@dfu-util -d 0483:df11 -s 0x90040000:leave -D $(BUILD_DIR)/wavex-daisy.bin
	@echo "DFU flash completed (if the device accepted the transfer)."
	@echo "Binary location: $(BUILD_DIR)/wavex-daisy.bin"

# Alias for flash
dfu: flash

# Build and flash with no button presses. The running firmware answers the
# trigger token on its USB CDC port by rebooting into the bootloader's DFU
# mode (see the DFU block in src/main.cpp), so dfu-util can take over. Needs
# firmware that is alive enough to service its main loop - if the board is
# wedged or blank, use the manual `flash` target instead.
DFU_TRIGGER := $(CMAKE_SOURCE_DIR)/../../scripts/daisy_dfu_trigger.py

flash-auto: bin
	@python3 $(DFU_TRIGGER)
	@dfu-util -d 0483:df11 -s 0x90040000:leave -D $(BUILD_DIR)/wavex-daisy.bin
	@echo "Flashed via software-triggered DFU (no buttons)."

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

