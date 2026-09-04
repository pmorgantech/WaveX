#!/bin/bash
# WaveX ESP32 Flash Script
# Convenient script for flashing ESP32 with correct port and baudrate

set -e

# Configuration. Resolve the ESP32 by USB VID:PID rather than a fixed ttyACM
# number: the Daisy re-enumerates on every reset and DFU cycle, so it can claim
# ACM0 and push the ESP32 to ACM1, and esptool then fails against the Daisy's
# CDC port with "No serial data received". Prefer the P4's built-in
# USB-Serial/JTAG port when it is plugged in, so the console on the CH343 UART
# bridge is left alone; fall back to the bridge otherwise. Override with
# ESP32_PORT=/dev/ttyACMn.
PORTS="$(dirname "$0")/scripts/serial_ports.py"
ESP32_PORT="${ESP32_PORT:-$(python3 "$PORTS" esp32-jtag 2>/dev/null || python3 "$PORTS" esp32)}"
ESP32_BAUDRATE="2000000"
ESP32_DIR="firmware/esp32"

echo "========================================================================"
echo "                    ⚡ WaveX ESP32 Flash Script"
echo "========================================================================"
echo "Port: $ESP32_PORT"
echo "Baudrate: $ESP32_BAUDRATE"
echo "Target: ESP32-P4"
echo "========================================================================"

# Check if ESP32 directory exists
if [ ! -d "$ESP32_DIR" ]; then
    echo "❌ Error: ESP32 directory not found: $ESP32_DIR"
    exit 1
fi

# Catches a stale explicit ESP32_PORT override; detection already failed loudly.
if [ ! -e "$ESP32_PORT" ]; then
    echo "❌ Error: Serial port not found: $ESP32_PORT"
    echo "Detected ports:"
    ls -l /dev/serial/by-id/ 2>/dev/null || echo "  none"
    exit 1
fi

# Navigate to ESP32 directory
cd "$ESP32_DIR"

# Source ESP-IDF environment
echo "🔧 Setting up ESP-IDF environment..."
source /opt/esp/idf/export.sh

# Flash the firmware
echo "⚡ Flashing ESP32 firmware..."
echo "Using port: $ESP32_PORT"
echo "Using baudrate: $ESP32_BAUDRATE"
echo ""

idf.py -p "$ESP32_PORT" -b "$ESP32_BAUDRATE" flash

echo ""
echo "✅ ESP32 firmware flashed successfully!"
echo "========================================================================"
