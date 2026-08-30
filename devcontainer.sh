#!/bin/bash
# Interactive shell in the WaveX devcontainer. ALL work happens in here -
# builds, tests, and git commits (the tracked .githooks/pre-commit wrapper
# blocks commits made outside the container).
#
# - ~/.gitconfig is mounted read-only so commits made inside have the host
#   identity (VS Code's Dev Containers does the same copy automatically).
# - The named volume keeps pre-commit's hook environments across runs, so
#   the first commit of a session doesn't re-download clang-format etc.
# - core.hooksPath is (re)pointed at .githooks on entry; the setting lands
#   in the bind-mounted .git/config, so a fresh clone is covered after its
#   first ./devcontainer.sh run.

docker run --rm -it \
    --privileged \
    --device=/dev/bus/usb \
    --volume=/dev:/dev \
    --group-add=plugdev \
    --group-add=dialout \
    -v "$PWD":/workspaces/WaveX \
    -v "$HOME/.gitconfig":/home/petem/.gitconfig:ro \
    -v wavex-precommit-cache:/home/petem/.cache/pre-commit \
    -w /workspaces/WaveX \
    wavex-devcontainer:latest \
    bash -c 'source /opt/esp/idf/export.sh >/dev/null 2>&1; \
             git config core.hooksPath .githooks 2>/dev/null; \
             exec bash'
