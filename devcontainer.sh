#!/bin/bash

docker run --rm -it \
    --privileged \
    --device=/dev/bus/usb \
    --volume=/dev:/dev \
    --group-add=plugdev \
    --group-add=dialout \
    -v "$PWD":/workspaces/WaveX \
    -w /workspaces/WaveX \
    wavex-devcontainer:latest \
    bash
