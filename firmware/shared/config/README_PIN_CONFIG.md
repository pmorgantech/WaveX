# WaveX Centralized Pin Configuration

## Overview

This directory contains the centralized pin configuration system for WaveX, which serves as the **single source of truth** for all hardware pin assignments across both ESP32 and Daisy platforms.

## 🎯 **Key Benefits**

- **Single Source of Truth**: All pin assignments defined in one place
- **Easy Verification**: Quickly verify pin assignments and detect conflicts
- **Simple Editing**: Change pins in one file, automatically applied everywhere
- **Platform Independence**: Works for both ESP32 and Daisy builds
- **Validation**: Built-in pin range validation and error checking

## 📁 **File Structure**

```
firmware/shared/config/
├── pin_config.h          # 🎯 CENTRALIZED: All pin assignments here
├── link_config.h         # 🔗 Link-specific configuration (includes pin_config.h)
└── README_PIN_CONFIG.md  # 📚 This documentation file
```

## 🔧 **How to Use**

### **1. View All Pin Assignments**

Open `pin_config.h` to see the complete pin mapping for both platforms:

```cpp
// ESP32-S3 Frontend Pin Assignments
#define WAVEX_ESP_SPI_SCLK      22   // J3-21: SPI3 SCLK to Daisy
#define WAVEX_ESP_SPI_MOSI      23   // J3-22: SPI3 MOSI to Daisy
#define WAVEX_ESP_SPI_MISO      24   // J3-23: SPI3 MISO from Daisy
#define WAVEX_ESP_SPI_CS        26   // J3-25: SPI3 CS to Daisy

// Daisy Seed Backend Pin Assignments  
#define WAVEX_DAISY_SPI_SCK     10   // D10: SPI1_SCK (clock from ESP32)
#define WAVEX_DAISY_SPI_MOSI    11   // D11: SPI1_MOSI (data from ESP32)
#define WAVEX_DAISY_SPI_MISO    12   // D12: SPI1_MISO (data to ESP32)
#define WAVEX_DAISY_SPI_CS      9    // D9: SPI1_NSS (chip select from ESP32)
```

### **2. Change Pin Assignments**

**⚠️ IMPORTANT**: Edit pins **ONLY** in `pin_config.h`:

```cpp
// Change this line in pin_config.h
#define WAVEX_ESP_SPI_SCLK      22   // J3-21: SPI3 SCLK to Daisy

// To this (for example)
#define WAVEX_ESP_SPI_SCLK      25   // J3-24: SPI3 SCLK to Daisy
```

The change automatically propagates to all other files that include `pin_config.h`.

### **3. Add New Peripherals**

Add new pin definitions to the appropriate section in `pin_config.h`:

```cpp
// New peripheral section
#define WAVEX_ESP_NEW_PERIPHERAL_CS    19   // J3-14: New peripheral CS
#define WAVEX_ESP_NEW_PERIPHERAL_SCLK  20   // J3-15: New peripheral SCLK
```

## 🔗 **How It Works**

### **ESP32 Side**

1. **`pin_config.h`** defines pin numbers (e.g., `WAVEX_ESP_SPI_SCLK = 22`)
2. **`hardware_pins.h`** includes `pin_config.h` and maps to GPIO_NUM macros
3. **`link_config.h`** includes `pin_config.h` and provides link-specific names
4. **Application code** uses the final GPIO_NUM definitions

### **Daisy Side**

1. **`pin_config.h`** defines pin numbers (e.g., `WAVEX_DAISY_SPI_SCK = 10`)
2. **`daisy_pins.h`** includes `pin_config.h` and maps to Daisy Pin objects
3. **Application code** uses the Pin objects (e.g., `WaveX::Pins::SPI_SCK`)

## 📍 **Current Pin Mapping**

### **ESP32-S3 (Frontend)**
- **Display**: GPIO4-7, 21 (ST7796S TFT)
- **Touch**: GPIO9, 14-15, 20 (FT6X36 I2C)
- **Inter-MCU SPI**: GPIO22-28 (SPI3 to Daisy)
- **SD Card**: GPIO10-13 (SPI2)
- **MIDI**: GPIO8, 42 (UART2)
- **Potentiometers**: GPIO1, 33-37 (CD74HC4067)
- **Buttons**: GPIO39-43 (TCA8418 I2C)
- **LEDs**: GPIO44-48 (TLC5947 SPI)

### **Daisy Seed (Backend)**
- **Inter-MCU SPI**: D9-14 (SPI1 from ESP32)
- **CV Outputs**: D25-30 (MCP4728 DACs)
- **Audio**: D21-24 (PCM1690 SAI2)
- **SD Card**: D17-20 (SPI)
- **Controls**: D15-16 (Analog inputs)
- **Available**: D0-8 (Unused pins)

## ✅ **Pin Validation**

The system includes built-in validation:

```cpp
// ESP32 pins must be 0-48
#define WAVEX_VALIDATE_ESP_PIN(pin) ((pin) >= 0 && (pin) <= 48)

// Daisy pins must be 0-30  
#define WAVEX_VALIDATE_DAISY_PIN(pin) ((pin) >= 0 && (pin) <= 30)
```

## 🚨 **Common Issues & Solutions**

### **Problem**: Pin conflicts between peripherals
**Solution**: Check `pin_config.h` for duplicate assignments

### **Problem**: Invalid pin numbers
**Solution**: Use pin validation macros and check pin ranges

### **Problem**: Changes not taking effect
**Solution**: Ensure you're editing `pin_config.h`, not other files

### **Problem**: Build errors after pin changes
**Solution**: Verify pin numbers are within valid ranges for each platform

## 🔄 **Migration Guide**

### **From Old System**

1. **Old**: Pin definitions scattered across multiple files
2. **New**: All pins defined in `pin_config.h`
3. **Benefit**: Single place to manage all pin assignments

### **To New System**

1. **Include**: `#include "shared/config/pin_config.h"`
2. **Use**: Platform-specific pin definitions (e.g., `WAVEX_ESP_SPI_SCLK`)
3. **Validate**: Use built-in validation macros

## 📚 **Related Files**

- **`firmware/esp32/components/hardware_config/include/hardware_pins.h`** - ESP32 GPIO mapping
- **`firmware/daisy/src/comm/daisy_pins.h`** - Daisy Pin object mapping
- **`firmware/shared/config/link_config.h`** - Link-specific configuration
- **`README.md`** - Project overview and pin assignments

## 🎯 **Best Practices**

1. **Always edit pins in `pin_config.h`**
2. **Use descriptive names** (e.g., `WAVEX_ESP_SPI_SCLK` not just `SCLK`)
3. **Add comments** with pin locations (e.g., `// J3-21: SPI3 SCLK`)
4. **Validate pin ranges** using built-in macros
5. **Test changes** by building both platforms
6. **Document pin changes** in commit messages

---

**⚠️ Remember**: This is the **single source of truth** for all pin assignments. Edit here, and changes propagate everywhere!
