FROM espressif/idf:release-v5.5

# STM32 / Daisy tool-chain
RUN apt-get update && apt-get install -y \
    gcc-arm-none-eabi gdb-multiarch udev \
    cmake ninja-build build-essential git python3-pip \
    openocd dfu-util stlink-tools sudo usbutils locales \
    curl ca-certificates \
    vim-tiny && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# LANG/LC_ALL are set below, but the base image ships no generated locale, so
# every shell prints "setlocale: LC_ALL: cannot change locale". Generate it.
RUN sed -i 's/^# *en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen && \
    locale-gen en_US.UTF-8

# Node.js + the CodeGraph CLI (see CLAUDE.md - `codegraph explore` is the
# preferred way to navigate this codebase). Pinned to the version used on the
# host so both environments read the same index format. Distro nodejs is too
# old for the CLI, hence NodeSource.
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - && \
    apt-get install -y nodejs && \
    npm install -g @colbymchenry/codegraph@1.4.1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# TyTools optional high-level ESP32 monitor
RUN pip install --no-cache-dir --break-system-packages esptool pyserial

# pre-commit lives in the image, not on the host: the hook suite (clang-format,
# firmware builds, host tests) assumes this container's toolchain, and the
# tracked .githooks/pre-commit wrapper refuses to run outside it. This env
# marker is what the wrapper keys on - it identifies "inside the WaveX
# devcontainer", so don't set it anywhere else.
RUN pip install --no-cache-dir --break-system-packages pre-commit
ENV WAVEX_DEVCONTAINER=1

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

# The rename above (usermod -l) only changes the login name, not the home
# directory or primary group name, so it can be left as /home/ubuntu:ubuntu.
# Normalize both to match the username so $HOME is predictable.
RUN set -e && \
    CURRENT_HOME=$(getent passwd petem | cut -d: -f6) && \
    if [ "$CURRENT_HOME" != "/home/petem" ]; then \
      usermod -d /home/petem -m petem; \
    fi && \
    CURRENT_GROUP=$(getent group 1000 | cut -d: -f1) && \
    if [ -n "$CURRENT_GROUP" ] && [ "$CURRENT_GROUP" != "petem" ]; then \
      groupmod -n petem "$CURRENT_GROUP"; \
    fi

ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# Run as petem by default so bind-mounted build output (build/, CMakeCache.txt,
# etc.) is owned by the host user instead of root. IDF_TOOLS_PATH/PATH are
# absolute (baked in via ENV in the base image, not $HOME-relative), so this
# is safe: petem only needs read access to /opt/esp, which the default image
# permissions already grant. VS Code's Dev Containers UID-remapping
# (updateRemoteUserUID + remoteUser in devcontainer.json) still overrides this
# for interactive sessions; this default matters for `docker run` / CI use.
USER petem
WORKDIR /home/petem

# Pre-create pre-commit's cache dir AS PETEM so the wavex-precommit-cache
# named volume (mounted here by devcontainer.sh) inherits this ownership on
# first use - a fresh named volume otherwise materializes root-owned and
# pre-commit dies on its .lock file.
RUN mkdir -p /home/petem/.cache/pre-commit
