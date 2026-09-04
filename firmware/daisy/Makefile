# WaveX Daisy Firmware Build Wrapper
# This Makefile configures CMake properly and delegates to the build directory

CMAKE_SOURCE_DIR := $(CURDIR)
# Overridable so CI can build a second flag-set configuration (roadmap Phase
# 1 item 1, e.g. BUILD_DIR=build-stageb) into its own directory without
# disturbing the default build/.
BUILD_DIR ?= build
# SRAM/debug images must never share the persistent QSPI build tree: the linker
# script, reset path, and section addresses are different.
DEBUG_BUILD_DIR ?= build-debug
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
DEBUG_ELF := $(DEBUG_BUILD_DIR)/wavex-daisy.elf
DEBUG_OPT ?= -O0
DEBUG_CMAKE_ARGS := -DDAISY_STORAGE=sram -DWAVEX_SRAM_DEBUG=ON -DCMAKE_BUILD_TYPE=Debug -DWAVEX_DAISY_OPT=$(DEBUG_OPT) -DFIRMWARE_DEBUG_OPT_LEVEL=$(DEBUG_OPT)
OPENOCD ?= openocd
OPENOCD_INTERFACE ?= interface/stlink.cfg
OPENOCD_TARGET ?= target/stm32h7x.cfg
GDB ?= gdb-multiarch
DEBUG_GDB_HOST ?= localhost
DEBUG_GDB_PORT ?= 3333
# Shared USB board resolver - nothing here should name a /dev/ttyACM number,
# since enumeration order shifts every time a board re-enumerates.
SERIAL_PORTS := $(CMAKE_SOURCE_DIR)/../../scripts/serial_ports.py
# How long `flash` waits for you to hold BOOT and tap RESET.
DFU_WAIT ?= 20

.PHONY: all build clean help configure stale-cache-check qspi-build-check
.PHONY: flash dfu sd usb bin run auto-flash flash-auto
.PHONY: debug debug-build debug-cache-check debug-clean debug-gdb-load debug-load debug-server
.PHONY: debug-probe-check debug-server-check

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

# The checked-in libDaisy SRAM mode is selected by DAISY_STORAGE=sram. WaveX's
# companion option makes that image self-starting after a direct SWD load (the
# Daisy bootloader is not involved) and selects the project-specific SRAM
# layout. Keep this in a fixed, separate cache from the default QSPI build.
debug-cache-check:
	@if [ -f "$(DEBUG_BUILD_DIR)/CMakeCache.txt" ]; then \
		storage=$$(sed -n 's/^DAISY_STORAGE:STRING=//p' "$(DEBUG_BUILD_DIR)/CMakeCache.txt"); \
		sram_debug=$$(sed -n 's/^WAVEX_SRAM_DEBUG:BOOL=//p' "$(DEBUG_BUILD_DIR)/CMakeCache.txt"); \
		build_type=$$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "$(DEBUG_BUILD_DIR)/CMakeCache.txt"); \
		wavex_opt=$$(sed -n 's/^WAVEX_DAISY_OPT:[^=]*=//p' "$(DEBUG_BUILD_DIR)/CMakeCache.txt"); \
		daisy_opt=$$(sed -n 's/^FIRMWARE_DEBUG_OPT_LEVEL:[^=]*=//p' "$(DEBUG_BUILD_DIR)/CMakeCache.txt"); \
		if [ "$$storage" != "sram" ] || [ "$$sram_debug" != "ON" ] \
			|| [ "$$build_type" != "Debug" ] || [ "$$wavex_opt" != "$(DEBUG_OPT)" ] \
			|| [ "$$daisy_opt" != "$(DEBUG_OPT)" ]; then \
			echo "Reconfiguring $(DEBUG_BUILD_DIR) for direct SRAM loading..."; \
			rm -rf "$(DEBUG_BUILD_DIR)"; \
		fi; \
	fi

debug-build: debug-cache-check
	@$(MAKE) BUILD_DIR="$(DEBUG_BUILD_DIR)" \
		CMAKE_EXTRA_ARGS="$(CMAKE_EXTRA_ARGS) $(DEBUG_CMAKE_ARGS)" build

# The SRAM path talks to the Daisy over SWD through an ST-Link, so the probe
# on USB is what proves the load can work - the Daisy's own CDC port may be
# absent (blank board, crashed image) and is not required. Resolved by VID:PID
# like the DFU targets, never by a /dev name.
debug-probe-check:
	@if ! python3 $(SERIAL_PORTS) --present stlink; then \
		echo "No ST-Link probe on USB. Connect it to the Daisy's SWD header and"; \
		echo "check it is passed through to the container (lsusb should list 0483:37xx)."; \
		exit 1; \
	fi

# debug-load relies on a GDB server somebody else started. Without this check
# GDB retries the connection for ~15 s, reports a timeout, then runs the rest
# of its batch script against no target and prints a page of noise.
debug-server-check:
	@if ! python3 -c "import socket,sys; socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2).close()" \
			"$(DEBUG_GDB_HOST)" "$(DEBUG_GDB_PORT)" >/dev/null 2>&1; then \
		echo "No GDB server on $(DEBUG_GDB_HOST):$(DEBUG_GDB_PORT)."; \
		echo "Start one with \`make debug-server\` in another shell, or use \`make debug\`"; \
		echo "which starts and stops its own OpenOCD."; \
		exit 1; \
	fi

# Internal loader shared by debug-load and the one-command debug target.
# Reset happens before the ELF load, never afterward: a post-load reset would
# return to the persistent QSPI image.
debug-gdb-load:
	@if ! command -v "$(GDB)" >/dev/null 2>&1; then \
		echo "Error: $(GDB) not found. Rebuild the WaveX devcontainer image."; \
		exit 1; \
	fi
	@$(GDB) --batch "$(DEBUG_ELF)" \
		-ex "target extended-remote $(DEBUG_GDB_HOST):$(DEBUG_GDB_PORT)" \
		-ex "monitor reset halt" \
		-ex "load" \
		-ex 'set $$sp = (unsigned int)&_estack' \
		-ex 'set $$pc = Reset_Handler' \
		-ex 'set *(unsigned int*)0xe000ed08 = 0x24000000' \
		-ex "monitor resume" \
		-ex "disconnect"
	@echo "SRAM image loaded and running; persistent QSPI firmware was not changed."

# Load into SRAM through an already-running OpenOCD GDB server, then run and
# disconnect.
debug-load: debug-probe-check debug-server-check debug-build
	@$(MAKE) DEBUG_BUILD_DIR="$(DEBUG_BUILD_DIR)" GDB="$(GDB)" \
		DEBUG_GDB_HOST="$(DEBUG_GDB_HOST)" DEBUG_GDB_PORT="$(DEBUG_GDB_PORT)" \
		debug-gdb-load

# One-command edit/test loop: start a temporary OpenOCD server, load the SRAM
# ELF via GDB, resume it, then stop the server while leaving the target running.
debug: debug-probe-check debug-build
	@if ! command -v "$(OPENOCD)" >/dev/null 2>&1; then \
		echo "Error: $(OPENOCD) not found. Rebuild the WaveX devcontainer image."; \
		exit 1; \
	fi
	@if ! command -v "$(GDB)" >/dev/null 2>&1; then \
		echo "Error: $(GDB) not found. Rebuild the WaveX devcontainer image."; \
		exit 1; \
	fi
	@set -eu; \
		log="$(DEBUG_BUILD_DIR)/openocd.log"; \
		"$(OPENOCD)" -f "$(OPENOCD_INTERFACE)" -f "$(OPENOCD_TARGET)" \
			-c "gdb_port $(DEBUG_GDB_PORT)" >"$$log" 2>&1 & \
		server_pid=$$!; \
		cleanup() { kill "$$server_pid" 2>/dev/null || true; wait "$$server_pid" 2>/dev/null || true; }; \
		trap cleanup EXIT INT TERM; \
		ready=0; \
		for attempt in $$(seq 1 50); do \
			if grep -q "Listening on port $(DEBUG_GDB_PORT) for gdb connections" "$$log"; then ready=1; break; fi; \
			if ! kill -0 "$$server_pid" 2>/dev/null; then cat "$$log"; exit 1; fi; \
			sleep 0.1; \
		done; \
		if [ $$ready -ne 1 ]; then echo "OpenOCD did not become ready:"; cat "$$log"; exit 1; fi; \
		if grep -q "examination failed" "$$log"; then \
			cat "$$log"; \
			echo "The ST-Link is up but the Daisy did not answer on SWD (see 'examination failed')."; \
			echo "Check the SWD wiring and board power, then re-run; this is intermittent on a"; \
			echo "probe that was interrupted mid-session."; \
			exit 1; \
		fi; \
		$(MAKE) DEBUG_BUILD_DIR="$(DEBUG_BUILD_DIR)" \
			DEBUG_GDB_HOST="$(DEBUG_GDB_HOST)" DEBUG_GDB_PORT="$(DEBUG_GDB_PORT)" \
			GDB="$(GDB)" debug-gdb-load

# Standalone server for an interactive GDB or Cortex-Debug session.
debug-server: debug-probe-check
	@if ! command -v "$(OPENOCD)" >/dev/null 2>&1; then \
		echo "Error: $(OPENOCD) not found. Rebuild the WaveX devcontainer image."; \
		exit 1; \
	fi
	@$(OPENOCD) -f "$(OPENOCD_INTERFACE)" -f "$(OPENOCD_TARGET)" \
		-c "gdb_port $(DEBUG_GDB_PORT)"

debug-clean:
	@$(MAKE) BUILD_DIR="$(DEBUG_BUILD_DIR)" clean

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f *.elf *.hex *.bin
	@echo "Clean complete"

# Refuse to turn the SRAM ELF into a DFU payload. The persistent path must
# always use the default QSPI linker layout and 0x90040000 destination.
qspi-build-check: build
	@storage=$$(sed -n 's/^DAISY_STORAGE:STRING=//p' "$(BUILD_DIR)/CMakeCache.txt"); \
	sram_debug=$$(sed -n 's/^WAVEX_SRAM_DEBUG:BOOL=//p' "$(BUILD_DIR)/CMakeCache.txt"); \
	if [ "$$storage" != "qspi" ] || [ "$$sram_debug" != "OFF" ]; then \
		echo "Error: $(BUILD_DIR) is not a persistent QSPI build; refusing to generate/flash a DFU binary."; \
		exit 1; \
	fi

# Build and generate the persistent QSPI binary file.
bin: qspi-build-check
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
	@echo "  make debug-build - Build the separate SRAM/debug ELF"
	@echo "  make debug    - Build, load to SRAM over ST-Link, and run"
	@echo "  make debug-load - Load/run via an existing OpenOCD server"
	@echo "  make debug-server - Start OpenOCD for GDB/Cortex-Debug"
	@echo "  make debug-clean - Remove the SRAM/debug build tree"
	@echo "  make bin      - Build and generate binary file"
	@echo "  make flash    - Persist the QSPI build via DFU (manual BOOT/RESET)"
	@echo "  make dfu      - Alias for flash"
	@echo "  make flash-auto - Persist the QSPI build via software-triggered DFU"
	@echo "  make auto-flash - Persist QSPI via automated boot switching + DFU"
	@echo "  make sd       - Build and prepare for SD card flashing"
	@echo "  make usb      - Build and prepare for USB drive flashing"
	@echo "  make run      - Build, flash, and start the Daisy"
	@echo "  make help     - Show this help message"
	@echo ""
	@echo "Build output: $(BUILD_DIR)/wavex-daisy.{elf,hex,bin}"
