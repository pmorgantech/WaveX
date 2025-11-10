FROM espressif/idf:release-v5.5

# STM32 / Daisy tool-chain
RUN apt-get update && apt-get install -y \
    gcc-arm-none-eabi udev \
    cmake ninja-build build-essential git python3-pip \
    openocd dfu-util stlink-tools sudo usbutils locales && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# TyTools optional high-level ESP32 monitor
RUN pip install --no-cache-dir --break-system-packages esptool pyserial

# Daisy libraries (for reference, actual code is in submodules)
RUN git clone --depth=1 https://github.com/electro-smith/libDaisy /opt/libDaisy && \
    git clone --depth=1 https://github.com/electro-smith/DaisySP /opt/DaisySP

# udev rules for USB access (user should provide these files if needed)
COPY 99-esp32.rules /etc/udev/rules.d/
COPY 49-daisy.rules /etc/udev/rules.d/
RUN udevadm control --reload-rules || true

# Add the user petem
# The base image may already have UID 1000, so check and handle
RUN set -e && \
    if id -u petem >/dev/null 2>&1; then \
      echo "User petem already exists"; \
    elif id 1000 >/dev/null 2>&1; then \
      EXISTING_USER=$(id -un 1000); \
      usermod -l petem "$EXISTING_USER" 2>/dev/null || true; \
    else \
      useradd -m -u 1000 -g 1000 -G sudo,plugdev,dialout -s /bin/bash petem; \
    fi && \
    (usermod -a -G sudo,plugdev,dialout petem 2>/dev/null || true) && \
    echo "petem ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers && \
    getent passwd petem

ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# Note: Do NOT set USER here - let devcontainer system handle user setup
# VSCode's UID adjustment layer needs to control the user configuration
