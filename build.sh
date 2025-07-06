#!/bin/bash

# WaveX Build Script
# Builds both ESP32 and Daisy firmware

set -e

echo "WaveX Dual-MCU Sampler/Synth Build Script"
echo "========================================"

# Check if we're running in a devcontainer or have the right tools
if [ -z "$IDF_PATH" ]; then
    echo "Error: ESP-IDF not found. Please run this in the devcontainer or install ESP-IDF"
    echo "Run: make devcontainer"
    exit 1
fi

# Check for ARM GCC
if ! command -v arm-none-eabi-gcc &> /dev/null; then
    echo "Error: ARM GCC not found. Please run this in the devcontainer or install arm-none-eabi-gcc"
    exit 1
fi

echo ""
echo "Building ESP32 firmware..."
echo "-------------------------"
cd firmware/esp32
idf.py build
cd ../..

echo ""
echo "Building Daisy firmware..."
echo "-------------------------"
mkdir -p firmware/daisy/build
cd firmware/daisy/build
cmake ..
make
cd ../../..

echo ""
echo "Build complete!"
echo "ESP32 binary: firmware/esp32/build/wavex-esp32.bin"
echo "Daisy binary: firmware/daisy/build/wavex-daisy.bin" 