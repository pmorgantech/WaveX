#!/bin/bash

# USB Device Setup Script for WaveX DevContainer
# This script helps detect and set up USB devices for ESP32 and Daisy

echo "🔍 Detecting USB devices..."

# Check for ESP32 devices
ESP32_DEVICES=$(lsusb | grep "303a:1001" || true)
if [ -n "$ESP32_DEVICES" ]; then
    echo "✅ ESP32 device detected:"
    echo "$ESP32_DEVICES"
    
    # Try to create device node
    echo "🔧 Attempting to create device node..."
    sudo mknod /dev/ttyACM0 c 166 0 2>/dev/null || true
    sudo chmod 666 /dev/ttyACM0 2>/dev/null || true
    
    if [ -e /dev/ttyACM0 ]; then
        echo "✅ /dev/ttyACM0 created successfully"
    else
        echo "⚠️  Could not create /dev/ttyACM0 - you may need to use USB-IP"
    fi
else
    echo "❌ No ESP32 devices detected"
fi

# Check for Daisy/STM32 devices
DAISY_DEVICES=$(lsusb | grep "0483:" || true)
if [ -n "$DAISY_DEVICES" ]; then
    echo "✅ Daisy/STM32 devices detected:"
    echo "$DAISY_DEVICES"
else
    echo "❌ No Daisy/STM32 devices detected"
fi

# List all available serial devices
echo "📋 Available serial devices:"
ls -la /dev/tty* 2>/dev/null || echo "No serial devices found"

echo "💡 If devices aren't working, try:"
echo "   1. Use Docker Desktop with USB-IP enabled"
echo "   2. Run container with: --device=/dev/bus/usb --privileged"
echo "   3. Install udev rules on host system" 