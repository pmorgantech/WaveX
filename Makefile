# WaveX Dual-MCU Sampler/Synth Build System
.PHONY: help all esp32 daisy daisy-stageb release esp32-release daisy-release check-release-clean check-profiles require-strings clean esp32-clean daisy-clean esp32-flash esp32-monitor esp32-flash-monitor esp32-menuconfig test test-all test-asan test-daisy test-esp32 test-shared test-clean ai-graph daisy-flash daisy-flash-auto flash-all start-logs stop-logs logs-start logs-stop

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

# Runs the same test bodies as `make test` with AddressSanitizer +
# UndefinedBehaviorSanitizer. This is a bug detector, not extra tests: an
# out-of-bounds read produces a plausible number under a normal build and the
# assertions still pass, so the existing suite executes those defects without
# observing them. Uses separate build-asan/ dirs so it never clobbers the
# ordinary test build's cache.
test-asan:
	@echo "========================================================================"
	@echo "            Running All WaveX Tests under ASan + UBSan"
	@echo "========================================================================"
	@rc=0; for suite in shared daisy esp32; do \
		echo ""; echo "--- $$suite (asan) ---"; \
		mkdir -p firmware/$$suite/tests/build-asan && \
		cd firmware/$$suite/tests/build-asan && \
		cmake -DWAVEX_TEST_SANITIZE=address .. >/dev/null && \
		$(MAKE) -j$$(nproc) >/dev/null || { rc=1; cd - >/dev/null; continue; }; \
		ctest --output-on-failure || rc=1; \
		cd - >/dev/null; \
	done; \
	if [ $$rc -ne 0 ]; then echo "SANITIZER FAILURES - see above"; fi; \
	exit $$rc
	@echo ""
	@echo "========================================================================"
	@echo "                    ALL SANITIZER TESTS COMPLETE"
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
	@rm -rf firmware/daisy/tests/build-asan
	@rm -rf firmware/esp32/tests/build-asan
	@rm -rf firmware/shared/tests/build-asan
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
	@echo "  release          - Build both MCUs in the release profile, then verify"
	@echo "  check-release-clean - Assert no debug console tokens in release images"
	@echo "  check-profiles   - Assert tokens present in debug AND absent in release"
	@echo "  esp32-clean      - Clean ESP32 build"
	@echo "  daisy-clean      - Clean Daisy build"
	@echo "  clean            - Clean all builds"
	@echo "  daisy-flash      - Flash Daisy via DFU (needs BOOT+RESET by hand)"
	@echo "  daisy-flash-auto - Flash Daisy with no button presses"
	@echo "  flash-all        - Stop logs, flash both MCUs in parallel, restart logs"
	@echo "  start-logs       - Rotate and start serial loggers"
	@echo "  stop-logs        - Stop serial loggers before flashing"
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
	@echo "Target: ESP32-P4 (UI, Controls, MIDI, Communication)"
	@echo "Toolchain: ESP-IDF release-v5.5"
	@echo "------------------------------------------------------------------------"
	# env -u GIT_*: `git commit` exports a RELATIVE GIT_INDEX_FILE
	# (.git/index) to its hooks. The IDF Component Manager checks git-sourced
	# components out via `git --work-tree /tmp/<tmp> --git-dir <cache>`, where
	# that inherited relative path resolves to /tmp/<tmp>/.git/index - which
	# does not exist - and the configure step dies with "Unable to create
	# index.lock". The firmware build must not see commit-time git env.
	cd firmware/esp32 && . /opt/esp/idf/export.sh && \
		env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE idf.py build
	@echo "------------------------------------------------------------------------"
	@echo "✅ ESP32 Frontend build completed successfully!"
	@echo "========================================================================"

esp32-clean:
	@echo "🧹 Cleaning ESP32 Frontend build..."
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py clean
	@echo "✅ ESP32 Frontend cleaned"

# ---------------------------------------------------------------------------
# ESP32 port resolution. A fixed /dev/ttyACM number is not stable: the Daisy
# re-enumerates on every reset and DFU cycle, so it can claim ACM0 and push the
# ESP32 to ACM1. esptool then talks to the Daisy's CDC port and fails with the
# unhelpful "No serial data received". Resolve by USB VID:PID instead, the same
# way the Daisy DFU trigger already does. Override with
# `make ESP32_PORT=/dev/ttyACMn` when you need to force a port.
# ---------------------------------------------------------------------------
ESP32_PORT ?=
ESP32_BAUD ?= 2000000
esp32_port = $(if $(ESP32_PORT),echo '$(ESP32_PORT)',python3 scripts/serial_ports.py esp32)

esp32-flash:
	@echo "⚡ Flashing ESP32 Frontend firmware..."
	@port=$$($(esp32_port)) && \
		echo "Port: $$port, Baudrate: $(ESP32_BAUD)" && \
		cd firmware/esp32 && . /opt/esp/idf/export.sh && \
		idf.py -p "$$port" -b $(ESP32_BAUD) flash
	@echo "✅ ESP32 Frontend flashed"

esp32-monitor:
	@echo "📺 Monitoring ESP32 Frontend..."
	@port=$$($(esp32_port)) && \
		echo "Port: $$port" && \
		cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py -p "$$port" monitor

esp32-menuconfig:
	@echo "⚙️  Configuring ESP32 Frontend..."
	cd firmware/esp32 && . /opt/esp/idf/export.sh && idf.py menuconfig

esp32-flash-monitor:
	@echo "⚡ Flashing and monitoring ESP32 Frontend..."
	@port=$$($(esp32_port)) && \
		echo "Port: $$port, Baudrate: $(ESP32_BAUD)" && \
		cd firmware/esp32 && . /opt/esp/idf/export.sh && \
		idf.py -p "$$port" -b $(ESP32_BAUD) flash monitor

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

# Compile the Stage B (TDM8/MCP48/8-group) flag set into a separate build
# dir, alongside the default Stage A build - proves both output/CV backend
# configurations compile (roadmap Phase 1 item 1 gate). Stage B has no real
# hardware yet (TdmVoiceSink/Mcp48Backend are stubs - see
# firmware/daisy/src/audio/output_sink.hpp / src/cv/mcp48_backend.hpp), so
# this only verifies compilation, not behavior.
daisy-stageb:
	@echo "========================================================================"
	@echo "            🎵 BUILDING DAISY SEED BACKEND (Stage B flag set)"
	@echo "========================================================================"
	cd firmware/daisy && make BUILD_DIR=build-stageb CMAKE_EXTRA_ARGS="-DWAVEX_VOICE_OUTPUT_BACKEND=1 -DWAVEX_CV_BACKEND=1 -DWAVEX_ANALOG_CV_GROUPS=8 -DWAVEX_ANALOG_CV_ENABLED=1"
	@echo "✅ Daisy Seed Backend (Stage B flag set) build completed successfully!"
	@echo "========================================================================"

# Release-profile builds (README.md#build-profiles). WAVEX_BUILD_DEBUG=0
# drops the console command surface - runtime log-level control on both boards,
# plus screenshots on the ESP32 - from the image. Each profile builds into its
# own directory: the Daisy wrapper only re-runs CMake configure when
# CMakeCache.txt is absent, so sharing build/ between profiles would silently
# keep whichever flags were configured first.
#
# WAVEX-ENTER-DFU deliberately survives into release; it is the only reflash
# path that needs no BOOT+RESET.
daisy-release:
	@echo "🎵 Building Daisy Seed Backend (release profile)..."
	cd firmware/daisy && $(MAKE) BUILD_DIR=build-release \
		CMAKE_EXTRA_ARGS="-DWAVEX_BUILD_DEBUG=OFF"
	@echo "✅ Daisy Seed Backend (release profile) built"

esp32-release:
	@echo "🔧 Building ESP32 Frontend (release profile)..."
	cd firmware/esp32 && . /opt/esp/idf/export.sh && \
		env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE \
		idf.py -B build-release -DWAVEX_BUILD_DEBUG=OFF build
	@echo "✅ ESP32 Frontend (release profile) built"

release: esp32-release daisy-release
	@$(MAKE) check-release-clean

# The gate that matters is not "release compiles" - it is that no console
# command token survives into the image. These are string literals, so they
# cannot outlive the code referencing them, and unlike a symbol they are not
# renamed by the linker or removed by inlining.
#
# A gate that can only ever pass is worthless, and this one has two ways to
# rot into exactly that: the tokens get renamed (so nothing is ever found), or
# `strings` is missing (an absent command produces no output, `grep -q` returns
# 1, and every check "passes"). require-strings closes the second;
# check-profiles below closes the first by asserting the tokens ARE present in
# a debug image.
DEBUG_TOKENS := WAVEX-LOG WAVEX-DBG WAVEX-SCREENSHOT
RELEASE_ELFS := firmware/daisy/build-release/wavex-daisy.elf \
                firmware/esp32/build-release/wavex-esp32.elf
DEBUG_ELFS := firmware/daisy/build/wavex-daisy.elf \
              firmware/esp32/build/wavex-esp32.elf

require-strings:
	@command -v strings >/dev/null 2>&1 || { \
		echo "❌ 'strings' not found - the token gate cannot run, refusing to report success"; \
		exit 1; }

check-release-clean: require-strings
	@rc=0; \
	for elf in $(RELEASE_ELFS); do \
		if [ ! -f "$$elf" ]; then echo "❌ missing $$elf - run 'make release' first"; rc=1; continue; fi; \
		for tok in $(DEBUG_TOKENS); do \
			if strings "$$elf" | grep -q "$$tok"; then \
				echo "❌ $$tok present in release image $$elf"; rc=1; \
			fi; \
		done; \
	done; \
	if [ $$rc -eq 0 ]; then echo "✅ no debug console tokens in either release image"; fi; \
	exit $$rc

# Both directions, so it needs all four images - which is why CI runs this and
# `make release` runs only the half that its own outputs can support. The
# positive half checks WAVEX-LOG alone: it is the one token present on both
# boards in a debug build, whereas WAVEX-SCREENSHOT is ESP32-only and
# WAVEX-DBG does not exist until the harness is built.
check-profiles: check-release-clean
	@rc=0; \
	for elf in $(DEBUG_ELFS); do \
		if [ ! -f "$$elf" ]; then echo "❌ missing $$elf - build the debug profile first"; rc=1; continue; fi; \
		if ! strings "$$elf" | grep -q 'WAVEX-LOG'; then \
			echo "❌ WAVEX-LOG absent from DEBUG image $$elf - the release gate proves nothing"; rc=1; \
		fi; \
	done; \
	if [ $$rc -eq 0 ]; then echo "✅ debug images carry the tokens, release images do not"; fi; \
	exit $$rc

# Add Daisy flash target for convenience
daisy-clean:
	@echo "🧹 Cleaning Daisy Seed Backend build..."
	cd firmware/daisy && make clean
	cd firmware/daisy && make BUILD_DIR=build-stageb clean
	@echo "✅ Daisy Seed Backend cleaned"

daisy-flash:
	@echo "⚡ Flashing Daisy Seed Backend firmware..."
	cd firmware/daisy && make flash
	@echo "✅ Daisy Seed Backend flashed"

# Touchless flash: no BOOT/RESET presses, no terminal to close first.
daisy-flash-auto:
	@echo "⚡ Flashing Daisy Seed Backend (software-triggered DFU)..."
	cd firmware/daisy && make flash-auto
	@echo "✅ Daisy Seed Backend flashed"

# Flash both MCUs concurrently. The serial ports must be free while flashing;
# logging is restarted only after both flash jobs have exited.
flash-all: stop-logs
	@set -eu; \
		daisy_status=0; esp32_status=0; \
		$(MAKE) daisy-flash-auto & daisy_pid=$$!; \
		$(MAKE) esp32-flash & esp32_pid=$$!; \
		wait $$daisy_pid || daisy_status=$$?; \
		wait $$esp32_pid || esp32_status=$$?; \
		$(MAKE) start-logs; \
		if [ $$daisy_status -ne 0 ] || [ $$esp32_status -ne 0 ]; then \
			echo "❌ One or more firmware flashes failed (Daisy=$$daisy_status ESP32=$$esp32_status)"; \
			exit 1; \
		fi

# ---------------------------------------------------------------------------
# Serial logging - replaces minicom so the ports stay scriptable.
# Each logger appends to a file you can `tail -f`, and reconnects by itself
# when a board resets or drops into DFU. Starting logs rotates old files first.
# ---------------------------------------------------------------------------
LOG_DIR ?= logs
LOG_KEEP ?= 4

start-logs: stop-logs
	@mkdir -p $(LOG_DIR)
	@for board in daisy esp32; do \
		log="$(LOG_DIR)/$$board.log"; \
		i=$(LOG_KEEP); \
		if [ $$i -gt 0 ]; then \
			rm -f "$$log.$$i"; \
			while [ $$i -gt 1 ]; do \
				prev=$$((i - 1)); \
				if [ -f "$$log.$$prev" ]; then cp "$$log.$$prev" "$$log.$$i"; fi; \
				i=$$prev; \
			done; \
			if [ -f "$$log" ]; then cp "$$log" "$$log.1"; : > "$$log"; fi; \
		else \
			rm -f "$$log"; \
		fi; \
	done
	@nohup python3 scripts/serial_log.py --vid 0483 --pid 5740 \
		--out $(LOG_DIR)/daisy.log --pidfile $(LOG_DIR)/daisy.pid >/dev/null 2>&1 &
	@nohup python3 scripts/serial_log.py \
		$(if $(ESP32_PORT),--port $(ESP32_PORT),--vid 1a86 --pid 55d3) --baud 115200 \
		--out $(LOG_DIR)/esp32.log --pidfile $(LOG_DIR)/esp32.pid >/dev/null 2>&1 &
	@sleep 1
	@echo "📝 Logging started:"
	@echo "   tail -f $(LOG_DIR)/daisy.log"
	@echo "   tail -f $(LOG_DIR)/esp32.log"

stop-logs:
	@for pidfile in $(LOG_DIR)/daisy.pid $(LOG_DIR)/esp32.pid; do \
		if [ -f "$$pidfile" ]; then \
			pid="$$(cat $$pidfile)"; \
			kill "$$pid" 2>/dev/null || true; \
			for attempt in $$(seq 1 20); do \
				if ! kill -0 "$$pid" 2>/dev/null; then break; fi; \
				sleep 0.1; \
			done; \
			rm -f "$$pidfile"; \
		fi; \
	done
	@echo "📝 Serial loggers stopped"

# Backward-compatible aliases for the original target names.
logs-start: start-logs
logs-stop: stop-logs

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

ai-graph:
	@if [ -d .codegraph ]; then \
		codegraph sync .; \
	else \
		codegraph init .; \
	fi

ai-graph-init:
	codegraph init .

ai-graph-rebuild:
	codegraph index --force .

ai-graph-status:
	codegraph status .
