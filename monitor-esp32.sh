#!/bin/bash
# WaveX ESP32 Monitor Script
# Convenient script for monitoring ESP32 serial output

set -e

# Configuration. Resolve the ESP32 by USB VID:PID rather than a fixed ttyACM
# number: the Daisy re-enumerates on every reset and DFU cycle, so it can claim
# ACM0 and push the ESP32 to ACM1, and esptool then fails against the Daisy's
# CDC port with "No serial data received". Override with ESP32_PORT=/dev/ttyACMn.
ESP32_PORT="${ESP32_PORT:-$(python3 "$(dirname "$0")/scripts/serial_ports.py" esp32)}"
ESP32_DIR="firmware/esp32"

echo "========================================================================"
echo "                    📺 WaveX ESP32 Monitor Script"
echo "========================================================================"
echo "Port: $ESP32_PORT"
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

# Monitor the firmware
echo "📺 Starting ESP32 monitor..."
echo "Using port: $ESP32_PORT"
echo ""
echo "Press Ctrl+] to exit monitor"
echo "========================================================================"

idf.py -p "$ESP32_PORT" monitor
