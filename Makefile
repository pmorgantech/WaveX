# WaveX Dual-MCU Sampler/Synth Build System
.PHONY: help all esp32 daisy clean esp32-clean daisy-clean esp32-flash esp32-monitor esp32-flash-monitor esp32-menuconfig test test-all test-daisy test-esp32 test-shared test-clean ai-graph

# Graphify
ai-graph:
	./scripts/graphify-refresh.sh

# Test targets
test: test-all

test-all:
	@echo "========================================================================"
	@echo "                    Running All WaveX Tests"
	@echo "========================================================================"
	@$(MAKE) test-shared
	@echo ""
	@$(MAKE) test-daisy
	@echo ""
	@$(MAKE) test-esp32
	@echo ""
	@echo "========================================================================"
	@echo "                         ALL TESTS COMPLETE"
	@echo "========================================================================"

test-daisy:
	@echo "Running Daisy Seed unit tests..."
	@if [ -d "firmware/daisy/tests" ]; then \
		rm -rf firmware/daisy/tests/build && \
		mkdir -p firmware/daisy/tests/build && \
		cd firmware/daisy/tests/build && \
		cmake .. && \
		make -j4 && \
		ctest --output-on-failure; \
	else \
		echo "Daisy tests directory not found - skipping"; \
	fi

test-esp32:
	@echo "Running ESP32 unit tests..."
	@if [ -d "firmware/esp32/tests" ]; then \
		rm -rf firmware/esp32/tests/build && \
		mkdir -p firmware/esp32/tests/build && \
		cd firmware/esp32/tests/build && \
		cmake .. && \
		make -j4 && \
		ctest --output-on-failure; \
	else \
		echo "ESP32 tests directory not found - skipping"; \
	fi

test-shared:
	@echo "Running shared protocol tests..."
	@if [ -d "firmware/shared/tests" ]; then \
		rm -rf firmware/shared/tests/build && \
		mkdir -p firmware/shared/tests/build && \
		cd firmware/shared/tests/build && \
		cmake .. && \
		make -j4 && \
		ctest --output-on-failure; \
	else \
		echo "Shared tests directory not found - skipping"; \
	fi

test-clean:
	@echo "Cleaning all test build artifacts..."
	@cd firmware/daisy && $(MAKE) test-clean || true
	@rm -rf firmware/esp32/tests/build
	@rm -rf firmware/shared/tests/build
	@echo "Test clean complete!"

# Default target
all: 
	@echo "========================================================================"
	@echo "                    WaveX Dual-MCU Build System"
	@echo "========================================================================"
	@echo "Building both ESP32 Frontend and Daisy Seed Backend..."
	@echo ""
	@$(MAKE) esp32
	@echo ""
	@$(MAKE) daisy
	@echo ""
	@echo "========================================================================"
	@echo "                         BUILD COMPLETE"
	@echo "========================================================================"
	@echo "✅ ESP32 Frontend (UI/Controls) - Built successfully"
	@echo "✅ Daisy Seed Backend (Audio Engine) - Built successfully"
	@echo ""
	@echo "Ready to flash:"
	@echo "  make esp32-flash    # Flash ESP32 firmware"
	@echo "  make daisy-flash    # Flash Daisy firmware (if supported)"
	@echo "========================================================================"

help:
	@echo "WaveX Build System"
	@echo "=================="
	@echo ""
	@echo "Available targets:"
	@echo "  all              - Build both ESP32 and Daisy firmware"
	@echo "  esp32            - Build ESP32 firmware"
	@echo "  daisy            - Build Daisy firmware"
	@echo "  esp32-clean      - Clean ESP32 build"
	@echo "  daisy-clean      - Clean Daisy build"
	@echo "  clean            - Clean all builds"
	@echo "  esp32-flash      - Flash ESP32 firmware"
	@echo "  esp32-monitor    - Monitor ESP32 serial output"
	@echo "  esp32-flash-monitor - Flash and monitor ESP32 (convenient)"
	@echo "  esp32-menuconfig - Configure ESP32 project"
	@echo "  test              - Run all tests"
	@echo "  test-daisy        - Run Daisy unit tests"
	@echo "  test-esp32        - Run ESP32 unit tests"
	@echo "  test-shared       - Run shared protocol tests"
	@echo "  test-clean        - Clean all test builds"
	@echo ""
	@echo "Note: Run these commands inside the devcontainer environment"

# ESP32 targets (using native ESP-IDF toolchain)
esp32:
	@echo "========================================================================"
	@echo "                    🔧 BUILDING ESP32 FRONTEND"
	@echo "========================================================================"
	@echo "Target: ESP32-S3 (UI, Controls, MIDI, Communication)"
	@echo "Toolchain: ESP-IDF v5.2"
	@echo "------------------------------------------------------------------------"
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py build
	@echo "------------------------------------------------------------------------"
	@echo "✅ ESP32 Frontend build completed successfully!"
	@echo "========================================================================"

esp32-clean:
	@echo "🧹 Cleaning ESP32 Frontend build..."
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py clean
	@echo "✅ ESP32 Frontend cleaned"

esp32-flash:
	@echo "⚡ Flashing ESP32 Frontend firmware..."
	@echo "Port: /dev/ttyACM0, Baudrate: 2000000"
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py -p /dev/ttyACM0 -b 2000000 flash
	@echo "✅ ESP32 Frontend flashed"

esp32-monitor:
	@echo "📺 Monitoring ESP32 Frontend..."
	@echo "Port: /dev/ttyACM0"
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py -p /dev/ttyACM0 monitor

esp32-menuconfig:
	@echo "⚙️  Configuring ESP32 Frontend..."
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py menuconfig

esp32-flash-monitor:
	@echo "⚡ Flashing and monitoring ESP32 Frontend..."
	@echo "Port: /dev/ttyACM0, Baudrate: 2000000"
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py -p /dev/ttyACM0 -b 2000000 flash monitor

# Daisy targets (using native ARM GCC toolchain)
daisy:
	@echo "========================================================================"
	@echo "                   🎵 BUILDING DAISY SEED BACKEND"
	@echo "========================================================================"
	@echo "Target: STM32H750 (Audio Engine, DSP, CV Output)"
	@echo "Toolchain: ARM GCC"
	@echo "------------------------------------------------------------------------"
	cd firmware/daisy && make
	@echo "------------------------------------------------------------------------"
	@echo "✅ Daisy Seed Backend build completed successfully!"
	@echo "========================================================================"

# Add Daisy flash target for convenience
daisy-clean:
	@echo "🧹 Cleaning Daisy Seed Backend build..."
	cd firmware/daisy && make clean
	@echo "✅ Daisy Seed Backend cleaned"

daisy-flash:
	@echo "⚡ Flashing Daisy Seed Backend firmware..."
	cd firmware/daisy && make flash
	@echo "✅ Daisy Seed Backend flashed"

# CMake-based builds (alternative to Make)
esp32-cmake:
	@echo "🔧 Building ESP32 Frontend with CMake..."
	cmake -B build/esp32 -S firmware/esp32 -G Ninja
	ninja -C build/esp32
	@echo "✅ ESP32 Frontend (CMake) built"

daisy-cmake:
	@echo "🎵 Building Daisy Seed Backend with CMake..."
	cmake -B build/daisy -S firmware/daisy -G Ninja
	ninja -C build/daisy
	@echo "✅ Daisy Seed Backend (CMake) built"

cmake-clean:
	@echo "🧹 Cleaning CMake builds..."
	rm -rf build/
	@echo "✅ CMake builds cleaned"

# Combined targets
clean: esp32-clean daisy-clean cmake-clean
	@echo "========================================================================"
	@echo "🧹 All builds cleaned successfully!"
	@echo "========================================================================"

# Development targets
setup:
	@echo "🚀 Setting up development environment..."
	git submodule update --init --recursive
	@echo "✅ Submodules updated successfully"

# Build system info
info:
	@echo "Build Environment Info:"
	@echo "======================="
	@echo "ESP-IDF Version: $$(idf.py --version 2>/dev/null || echo '❌ Not found')"
	@echo "ARM GCC Version: $$(arm-none-eabi-gcc --version 2>/dev/null | head -1 || echo '❌ Not found')"
	@echo "CMake Version: $$(cmake --version 2>/dev/null | head -1 || echo '❌ Not found')"
	@echo "Ninja Version: $$(ninja --version 2>/dev/null || echo '❌ Not found')" 
