FROM espressif/idf:release-v5.2

# STM32 / Daisy tool-chain
RUN apt-get update && apt-get install -y \
    gcc-arm-none-eabi \
    cmake ninja-build build-essential git python3-pip \
    openocd dfu-util stlink-tools

# Install stlink tools for STM32 programming/debugging
RUN apt-get update && apt-get install -y stlink-tools

# TyTools optional high-level ESP32 monitor
RUN pip install --no-cache-dir esptool pyserial

# Daisy libraries (for reference, actual code is in submodules)
RUN git clone --depth=1 https://github.com/electro-smith/libDaisy /opt/libDaisy && \
    git clone --depth=1 https://github.com/electro-smith/DaisySP /opt/DaisySP

# udev rules for USB access (user should provide these files if needed)
# COPY 99-esp32.rules /etc/udev/rules.d/
# COPY 49-daisy.rules /etc/udev/rules.d/
RUN udevadm control --reload-rules || true

# Set up user for VS Code
RUN usermod -aG dialout vscode || true 