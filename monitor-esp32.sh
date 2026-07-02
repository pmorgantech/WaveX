#!/bin/bash
# WaveX ESP32 Monitor Script
# Convenient script for monitoring ESP32 serial output

set -e

# Configuration
ESP32_PORT="/dev/ttyACM0"
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

# Check if port exists
if [ ! -e "$ESP32_PORT" ]; then
    echo "❌ Error: Serial port not found: $ESP32_PORT"
    echo "Available ports:"
    ls /dev/tty* | grep -E "(ACM|USB|S)" | head -5 || echo "  No ports found"
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
