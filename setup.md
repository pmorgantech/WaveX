Below is a one-stop, **drop-in dev-container guide** that turns any VS Code install into a reproducible build-&-debug cockpit for your dual-MCU sampler/synth:

* **ESP32-P4-WIFI6 side:** ESP-IDF, LVGL (MIPI-DSI display + GT911 touch), SD-MMC, USB storage/MIDI, SPI link to the Daisy.
* **Daisy Seed side:** libDaisy + DaisySP on an STM32H750, SPI bridge back to the ESP32, I²S audio out.

Everything lives in one Docker image, so new contributors clone the repo, hit **“Re-open in Container”**, and start coding—no tool-chain hunts, no “works on my machine.”

---

## Docker Installation & Build Instructions (Ubuntu/Mint)

To use the devcontainer, you need Docker and Buildx support. On Ubuntu/Mint, install the following packages:

```sh
sudo apt update
sudo apt install docker.io docker-compose-v2 docker-buildx-plugin docker-registry
```

- After installing, ensure your user is in the `docker` group:
  ```sh
  sudo usermod -aG docker $USER
  # Log out and log back in, or reboot, to apply group changes
  ```
- Enable and start the Docker service:
  ```sh
  sudo systemctl enable --now docker
  ```
- (Optional but recommended) Enable BuildKit for modern builds:
  ```sh
  export DOCKER_BUILDKIT=1
  # Add to ~/.bashrc or ~/.profile for persistence
  ```
- Verify Buildx is available:
  ```sh
  docker buildx version
  ```

### Building the Devcontainer Image Manually

From the project root:
```sh
docker buildx build -f .devcontainer/Dockerfile -t wavex-devcontainer .
```
Or, to load the image into your local Docker for use with `docker run`:
```sh
docker buildx build --load -f .devcontainer/Dockerfile -t wavex-devcontainer .
```

---

## Dev-container architecture

### Why shove it all in Docker?

* **Exact tool versions:** the Espressif `espressif/idf` image already bundles a frozen ESP-IDF + Python dependencies for any release tag ([docs.espressif.com][1]).
* **Clean ARM tool-chain for Daisy:** we add `gcc-arm-none-eabi`, `cmake`, `ninja`, `dfu-util`, and OpenOCD on top ([community.st.com][2], [openocd.org][3]).
* **USB ready:** containers can talk to real boards when started with `--device` or the new USB-IP option in Docker Desktop 4.29+ ([stackoverflow.com][4], [docs.docker.com][5]).
* **VS Code Dev Containers extension** mounts your repo, pre-installs extensions, and shares a Linux shell—all declared in a `devcontainer.json` file ([code.visualstudio.com][6], [code.visualstudio.com][7]).

### Folder layout

```
/firmware/
  ├── esp32/
  │   ├── main/         # Main application code (entry point, high-level logic)
  │   ├── components/   # Custom reusable modules (drivers, protocol, etc.)
  │   ├── libs/         # ESP32-specific external dependencies (submodules, e.g., LVGL)
  │   ├── sdkconfig
  │   └── CMakeLists.txt
  ├── daisy/
  │   ├── src/          # Daisy backend application code
  │   ├── libs/         # Daisy-specific external dependencies (submodules, e.g., DaisySP)
  │   └── CMakeLists.txt
  └── shared/
      ├── spi_protocol/
      ├── utils/
      └── CMakeLists.txt
/docs/                 # Architecture, setup, and documentation
/.devcontainer/        # Dockerfile and devcontainer.json
/README.md             # Project overview and quickstart
/setup.md              # Detailed setup and workflow guide
```

---

## External Dependencies & Submodules

External libraries (such as LVGL for ESP32 or DaisySP for Daisy) are managed as git submodules in the appropriate `libs/` directory.

- To initialize submodules after cloning:
  ```sh
  git submodule update --init --recursive
  ```
- To add a new submodule (example: LVGL for ESP32):
  ```sh
  cd firmware/esp32/libs
  git submodule add https://github.com/lvgl/lvgl.git
  ```
- To update all submodules:
  ```sh
  git submodule update --remote --merge
  ```

**Note:** CMakeLists.txt in each firmware directory should be updated to include the relevant libs as needed.

---

## Dockerfile (essentials)

```Dockerfile
FROM espressif/idf:release-v5.5       # ESP32 tool-chain + Python 3   (turn0search1)

# STM32 / Daisy tool-chain
RUN apt-get update && apt-get install -y \
    gcc-arm-none-eabi \
    cmake ninja-build build-essential git python3-pip \
    openocd dfu-util                                           # Flash+debug tools (turn0search5, turn0search13)

# TyTools optional high-level ESP32 monitor
RUN pip install --no-cache-dir esptool pyserial

# Daisy libraries
RUN git clone --depth=1 https://github.com/electro-smith/libDaisy /opt/libDaisy && \
    git clone --depth=1 https://github.com/electro-smith/DaisySP /opt/DaisySP  # (turn0search11)

# udev rules for USB access
COPY 99-esp32.rules /etc/udev/rules.d/   # from ESP-IDF docs (turn0search7)
COPY 49-daisy.rules /etc/udev/rules.d/   # DFU/STM32 rules (turn0search4)

RUN udevadm control --reload-rules
```

---

## `devcontainer.json`

```jsonc
{
  "name": "Dual-MCU-Synth",
  "build": { "dockerfile": "Dockerfile" },
  "runArgs": [
    "--privileged",
    "--device=/dev/bus/usb",          // pass all USB (turn0search14)
    "--device=/dev/ttyACM0",          // ESP32 CDC
    "--device=/dev/ttyACM1"           // Daisy DFU serial (optional)
  ],
  "postStartCommand": "udevadm trigger --subsystem-match=usb",
  "extensions": [
    "ms-vscode.cpptools",
    "marus25.cortex-debug",           // STM32/OpenOCD (turn0search5)
    "espressif.esp-idf-extension-pack"
  ],
  "features": {
    "ghcr.io/devcontainers/features/docker-in-docker:2": {}
  }
}
```

If you’re on macOS/Windows, swap the `--device` lines for `"mounts": ["source=/var/run/docker-host, …"]` and forward USB with Docker Desktop’s USB-IP wizard ([docs.docker.com][5]).

---

## Build / flash workflow

### Unified Build System (Recommended)

WaveX uses a dual build system approach with a top-level Makefile that orchestrates both ESP32 and Daisy builds:

| Command | Description | Build System Used |
|---------|-------------|------------------|
| `make all` | Build both ESP32 and Daisy with enhanced visual output | CMake + Make |
| `make esp32` | Build ESP32 frontend only | CMake via idf.py |
| `make daisy` | Build Daisy backend only | Make |
| `make clean` | Clean both build systems | Both |
| `make setup` | Initialize git submodules | N/A |
| `make info` | Show toolchain versions | N/A |

**Enhanced Visual Output**: The unified Makefile provides clear progress indicators showing which MCU is being built:

```
========================================================================
                    WaveX Dual-MCU Build System
========================================================================
Building both ESP32 Frontend and Daisy Seed Backend...

========================================================================
                    🔧 BUILDING ESP32 FRONTEND
========================================================================
Target: ESP32-P4 (UI, Controls, MIDI, Communication)
Toolchain: ESP-IDF v5.5
------------------------------------------------------------------------
[ESP-IDF build output]
------------------------------------------------------------------------
✅ ESP32 Frontend build completed successfully!
========================================================================

========================================================================
                   🎵 BUILDING DAISY SEED BACKEND
========================================================================
Target: STM32H750 (Audio Engine, DSP, CV Output)
Toolchain: ARM GCC
------------------------------------------------------------------------
[ARM GCC build output]
------------------------------------------------------------------------
✅ Daisy Seed Backend build completed successfully!
========================================================================
```

### ESP32-P4 Frontend (Individual Commands)

| Step          | Command                                | Notes                                                                           |
| ------------- | -------------------------------------- | ------------------------------------------------------------------------------- |
| **Configure** | `idf.py set-target esp32p4`            | Done once.                                                                      |
| **Build**     | `idf.py build`                         | Uses ESP-IDF's CMake build system ([docs.espressif.com][8])                          |
| **Flash**     | `idf.py -p /dev/ttyACM0 flash monitor` | Works inside container when `--device` passed.                                  |
| **DFU alt**   | `idf.py dfu-flash`                     | P4 can flash via native USB DFU if UART pins are busy ([docs.espressif.com][9]) |

### Daisy Seed Backend (Individual Commands)

| Step              | Command                                                                                                      | Notes                                                       |
| ----------------- | ------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------- |
| **Build**         | `make` (from firmware/daisy directory)                                                                       | Uses libDaisy Makefile templates ([github.com][10])                   |
| **Flash (quick)** | `dfu-util -a 0 -D build/myfirmware.bin`                                                                      | Seed bootloader expects DFU ([forum.electro-smith.com][11]) |
| **Flash (debug)** | `openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c "program build/myfirmware.elf verify reset exit"` | Requires ST-Link or CMSIS-DAP ([community.st.com][2])       |
| **GDB attach**    | Cortex-Debug launch: `"servertype": "openocd", "gdbTarget": "localhost:3333"`                                | Full halt-mode stepping.                                    |

### Build System Architecture

- **ESP32 Frontend**: Uses ESP-IDF's native CMake-based build system via `idf.py`
  - Component-based architecture with automatic dependency resolution
  - Built-in support for partitions, bootloader, and flash configuration
  - Integrated menuconfig for project configuration

- **Daisy Backend**: Uses traditional Make with libDaisy Makefile templates
  - Direct integration with libDaisy and DaisySP libraries
  - Optimized compiler flags for STM32H750 performance
  - Support for both DFU and OpenOCD/ST-Link flashing

- **Orchestration**: Top-level Makefile coordinates both builds
  - Automatically sources ESP-IDF environment (`/opt/esp/idf/export.sh`)
  - Enhanced visual output with clear progress indicators
  - No Docker-in-Docker overhead - all builds run natively in devcontainer

---

## Display, touch & peripherals

* **LVGL 9** runs fine on ESP32-P4 + MIPI-DSI display; see official LVGL-ESP32 examples ([hackster.io][12]).
* GT911 capacitive touch controller uses I2C interface with interrupt support for reliable touch detection.
* SD-MMC and USB-MSC sample projects (`storage/sd_card` and `usb/host/msc`) are included in ESP-IDF 5.x ([docs.espressif.com][8]).
* USB MIDI uses the TinyUSB class already wrapped by ESP-IDF (`tinyusb_midi_streaming` example) ([docs.espressif.com][8]).
* DaisySP handles oscillators & sample playback; the ESP32 streams commands over SPI (8- or 16-bit frames) using the `spi_master` half-duplex driver ([github.com][10]).

---

## Unit & integration tests

* **Host-only logic**: GoogleTest built and run in container.
* **ESP32 hardware**: `idf.py flash monitor -T` runs Unity tests from RAM; results appear on monitor.
* **Daisy hardware**: LibDaisy’s CMake helpers register each test binary; a Raspberry Pi can compile ↔ flash ↔ read pass/fail automatically ([forum.electro-smith.com][13]).

---

## Continuous integration hook

```yaml
# .github/workflows/build.yml
jobs:
  docker-build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: devcontainers/ci@v1
        with:
          runCmd: |
            idf.py build            # build esp32
            cmake -B build && ninja -C build   # build daisy
```

The action reuses the same `.devcontainer` image, so CI and local dev never drift ([community.st.com][2]).

---

## Pro tips & pitfalls

* **Serial group:** add `usermod -aG dialout vscode` in the Dockerfile so the container user sees `/dev/ttyACM*` ([stackoverflow.com][4]).
* **Hot-plug:** Docker on Linux accepts new USB devices without restart; on macOS/Windows you must re-open the container or use USB-IP ([docs.docker.com][5]).
* **Clock sanity:** keep LVGL at 30 FPS to leave SPI bandwidth for the Daisy link.
* **SPI framing:** reserve byte 0 as “command” and byte 1..n as payload to keep code identical on both MCUs.
* **Latency watch:** use the DWT cycle counter on STM32 and `esp_timer_get_time()` on ESP32 to measure end-to-end control latency (goal ≤ 1 ms).

---

### You’re ready

Commit the `.devcontainer` folder, push, and tell collaborators “Docker + VS Code, then hit F1 → *Reopen in Container*.” They’ll get the same ESP-IDF, the same ARM GCC, the same flashing scripts—and your synth’s firmware will finally stop depending on who built it.

[1]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/tools/idf-docker-image.html?utm_source=chatgpt.com "IDF Docker Image - ESP32 - Espressif Systems"
[2]: https://community.st.com/t5/stm32-mcus-embedded-software/port-custom-stm32h750-external-flash-loader-stldr-to-a-openocd/td-p/132913?utm_source=chatgpt.com "port custom stm32h750 external flash loader stldr"
[3]: https://openocd.org/doc/html/Flash-Commands.html?utm_source=chatgpt.com "Flash Commands (OpenOCD User's Guide)"
[4]: https://stackoverflow.com/questions/24225647/docker-a-way-to-give-access-to-a-host-usb-or-serial-device?utm_source=chatgpt.com "Docker - a way to give access to a host USB or serial device?"
[5]: https://docs.docker.com/desktop/features/usbip/?utm_source=chatgpt.com "USB/IP support - Docker Docs"
[6]: https://code.visualstudio.com/docs/devcontainers/containers?utm_source=chatgpt.com "Developing inside a Container - Visual Studio Code"
[7]: https://code.visualstudio.com/docs/devcontainers/tutorial?utm_source=chatgpt.com "Dev Containers tutorial - Visual Studio Code"
[8]: https://docs.espressif.com/projects/vscode-esp-idf-extension/en/latest/additionalfeatures/docker-container.html?utm_source=chatgpt.com "Using Docker Container - - — ESP-IDF Extension for VSCode latest ..."
[9]: https://docs.espressif.com/projects/esp-idf/en/v5.0.2/esp32p4/api-guides/dfu.html?utm_source=chatgpt.com "Device Firmware Upgrade via USB - ESP32-P4"
[10]: https://github.com/electro-smith/libDaisy?utm_source=chatgpt.com "electro-smith/libDaisy: Hardware Library for the Daisy Audio Platform"
[11]: https://forum.electro-smith.com/t/seed-bootloader-dfu-util/3743?utm_source=chatgpt.com "Seed bootloader + dfu-util - Software Development - Daisy Forums"
[12]: https://www.hackster.io/leoribg/esp-idf-in-docker-dev-container-e8510f?utm_source=chatgpt.com "ESP-IDF in Docker Dev Container - Hackster.io"
[13]: https://forum.electro-smith.com/t/libdaisy-automated-hardware-tests/1686?utm_source=chatgpt.com "libDaisy: Automated hardware tests - Daisy Forums"
