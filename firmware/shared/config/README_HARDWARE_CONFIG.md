# WaveX Hardware Configuration and Logging System

This document describes the comprehensive hardware component configuration and logging system implemented for the WaveX project.

## Overview

The WaveX project now includes a centralized configuration system that allows:

1. **Selective hardware component enable/disable** at compile time
2. **Granular logging control** for each component
3. **Platform-specific configuration** for Daisy and ESP32
4. **Runtime configuration options** for certain components

## File Structure

```
firmware/
├── shared/
│   └── config/
│       ├── hardware_config.h          # ALL hardware component configuration
│       ├── logging_config.h           # Shared logging configuration
│       ├── pin_config.h               # Pin assignments (existing)
│       └── link_config.h              # Inter-MCU link configuration (existing)
├── daisy/
│   └── src/
│       └── config.hpp                 # Daisy-specific configuration + shared includes
└── esp32/
    └── main/
        └── config.h                   # ESP32-specific configuration + shared includes
```

## Hardware Component Configuration

**All hardware component configuration is now centralized in `firmware/shared/config/hardware_config.h`**

### Shared Components

These components are used by both Daisy and ESP32:

- **Inter-MCU Communication Link** (`WAVEX_INTER_MCU_LINK_ENABLED`)
  - Controls SPI/UART communication between Daisy and ESP32
  - Must be enabled if any component depends on inter-MCU communication

### Daisy-Specific Components

- **Audio Engine** (`WAVEX_AUDIO_ENGINE_ENABLED`)
  - Controls the entire audio processing pipeline
  - Can be completely disabled or muted via `WAVEX_AUDIO_OUTPUT_ENABLED`
  - Audio input can be controlled via `WAVEX_AUDIO_INPUT_ENABLED`
  - Configurable sample rate, block size, buffer size, and priority

- **DAC CV Outputs** (`WAVEX_DAC_CV_OUTPUTS_ENABLED`)
  - Controls the MCP4728 DAC outputs for CV control
  - Configurable resolution, voltage range, and update rate

- **USB Configuration** (`WAVEX_DAISY_USB_ENABLED`)
  - Controls USB functionality on Daisy

- **SD Card** (`WAVEX_DAISY_SD_CARD_ENABLED`)
  - Controls SD card functionality (currently disabled due to SPI conflicts)

- **External Flash** (`WAVEX_DAISY_EXTERNAL_FLASH_ENABLED`)
  - Controls external flash memory access

### ESP32-Specific Components

- **Quadrature Encoder** (`WAVEX_ENCODER_PCNT_ENABLED`)
  - Controls the PCNT peripheral for encoder input
  - Configurable via `WAVEX_ENCODER_INPUT_ENABLED` and `WAVEX_ENCODER_IRQ_ENABLED`
  - Includes PCNT unit, channel, and filter configuration

- **CD74HC4067 Mux** (`WAVEX_4067_MUX_ENABLED`)
  - Controls the 16-channel analog multiplexer
  - Currently disabled (using encoder instead)
  - Includes ADC and address pin configuration

- **TCA8418 Button Matrix** (`WAVEX_TCA8418_BUTTON_MATRIX_ENABLED`)
  - Controls the 8x8 capacitive button matrix
  - Currently disabled (using encoder instead)
  - Includes I2C configuration and task settings

- **ST7796S LCD Display** (`WAVEX_LCD_DISPLAY_ENABLED`)
  - Controls the 480x320 TFT display
  - Configurable backlight and touch support
  - Includes resolution, color depth, and SPI configuration

- **USB MIDI Interface** (`WAVEX_USB_MIDI_ENABLED`)
  - Controls USB MIDI input/output
  - Configurable buffer sizes and task priorities

- **PSRAM** (`WAVEX_ESP_PSRAM_ENABLED`)
  - Controls PSRAM usage on ESP32

- **WiFi** (`WAVEX_ESP_WIFI_ENABLED`)
  - Controls WiFi functionality (currently disabled)

- **Bluetooth** (`WAVEX_ESP_BLUETOOTH_ENABLED`)
  - Controls Bluetooth functionality (currently disabled)

## Logging Configuration

### Global Logging Control

- **`WAVEX_DEBUG_LOGGING_ENABLED`** - Master switch for all debug logging
- Set to `0` to completely disable all debug output for production builds

### Component-Specific Logging

Each component has its own logging macro:

- **`WAVEX_LOG_INTER_MCU_LINK`** - Inter-MCU communication logging
- **`WAVEX_LOG_AUDIO_ENGINE`** - Audio engine logging
- **`WAVEX_LOG_DAC_CV_OUTPUTS`** - DAC CV outputs logging
- **`WAVEX_LOG_ENCODER`** - Encoder logging
- **`WAVEX_LOG_4067_MUX`** - 4067 mux logging
- **`WAVEX_LOG_TCA8418_BUTTON_MATRIX`** - Button matrix logging
- **`WAVEX_LOG_LCD_DISPLAY`** - LCD display logging
- **`WAVEX_LOG_USB_MIDI`** - USB MIDI logging

### Platform-Specific Logging Macros

#### ESP32 Logging
```cpp
WAVEX_LOG_ESP_INFO(LCD_DISPLAY, "Display initialized");
WAVEX_LOG_ESP_WARN(ENCODER, "Encoder input error");
WAVEX_LOG_ESP_ERROR(USB_MIDI, "MIDI initialization failed");
```

#### Daisy Logging
```cpp
WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio callback executed");
WAVEX_LOG_DAISY(INTER_MCU_LINK, "SPI packet received");
WAVEX_LOG_DAISY(DAC_CV_OUTPUTS, "CV output updated");
```

### Convenience Logging Macros

```cpp
// Component initialization
WAVEX_LOG_INIT(LCD_DISPLAY, "ST7796S display");
WAVEX_LOG_INIT_SUCCESS(ENCODER, "PCNT peripheral");
WAVEX_LOG_INIT_FAILED(USB_MIDI, "USB descriptor");

// State changes and errors
WAVEX_LOG_STATE_CHANGE(AUDIO_ENGINE, "Sample rate changed to %d", sample_rate);
WAVEX_LOG_ERROR(INTER_MCU_LINK, "SPI communication timeout");
WAVEX_LOG_WARN(DAC_CV_OUTPUTS, "CV output out of range");
```

## Usage Examples

### Enabling/Disabling Components

To disable the audio engine completely:

```cpp
// In hardware_config.h or via compiler define
#define WAVEX_AUDIO_ENGINE_ENABLED 0
```

To disable LCD display logging:

```cpp
// In hardware_config.h or via compiler define
#define WAVEX_LOG_LCD_DISPLAY 0
```

### Runtime Configuration

To mute audio output while keeping the engine running:

```cpp
#define WAVEX_AUDIO_ENGINE_ENABLED 1
#define WAVEX_AUDIO_OUTPUT_ENABLED 0
```

### Conditional Compilation

```cpp
#if WAVEX_ENCODER_PCNT_ENABLED
    // Encoder-specific code
    pcnt_config_t config = {
        .unit = WAVEX_ENCODER_PCNT_UNIT,
        .channel = WAVEX_ENCODER_PCNT_CH_A,
        // ... other config
    };
    pcnt_unit_config(&config);
#endif
```

### Logging Control

```cpp
// Only execute logging code when component logging is enabled
WAVEX_IF_LOGGING(LCD_DISPLAY, {
    ESP_LOGI(TAG, "Display refresh rate: %d Hz", refresh_rate);
});
```

## Compiler Definitions

You can override any configuration macro via compiler definitions:

```bash
# Disable all debug logging
make CFLAGS="-DWAVEX_DEBUG_LOGGING_ENABLED=0"

# Disable specific components
make CFLAGS="-DWAVEX_AUDIO_ENGINE_ENABLED=0 -DWAVEX_LCD_DISPLAY_ENABLED=0"

# Disable specific logging
make CFLAGS="-DWAVEX_LOG_AUDIO_ENGINE=0 -DWAVEX_LOG_LCD_DISPLAY=0"
```

## Migration Guide

### From Old System

1. **Replace direct `hw.PrintLine` calls** with `WAVEX_LOG_DAISY(component, ...)`
2. **Replace direct `ESP_LOGI` calls** with `WAVEX_LOG_ESP_INFO(component, ...)`
3. **Wrap hardware initialization** with `#if WAVEX_<COMPONENT>_ENABLED` checks
4. **All configuration is now in** `firmware/shared/config/hardware_config.h`

### Example Migration

**Before:**
```cpp
hw.PrintLine("Audio engine initialized");
ESP_LOGI(TAG, "Display initialized");
```

**After:**
```cpp
WAVEX_LOG_DAISY(AUDIO_ENGINE, "Audio engine initialized");
WAVEX_LOG_ESP_INFO(LCD_DISPLAY, "Display initialized");
```

## Best Practices

1. **All hardware configuration** goes in `firmware/shared/config/hardware_config.h`
2. **Platform-specific config files** only contain platform-specific settings (UART baud rates, etc.)
3. **Always check component enable flags** before initializing hardware
4. **Use appropriate logging levels** (INFO, WARN, ERROR, DEBUG)
5. **Test with components disabled** to ensure clean compilation

## Troubleshooting

### Common Issues

1. **"Component not found" errors** - Check that the component is properly defined in `hardware_config.h`
2. **Logging not working** - Verify `WAVEX_DEBUG_LOGGING_ENABLED` is set to 1
3. **Hardware not initializing** - Check that the component's `_ENABLED` macro is set to 1

### Debug Configuration

To enable verbose logging for troubleshooting:

```cpp
#define WAVEX_DEBUG_LOGGING_ENABLED 1
#define WAVEX_LOG_INTER_MCU_LINK 1
#define WAVEX_LOG_AUDIO_ENGINE 1
// ... enable other components as needed
```

## Future Enhancements

- **Runtime configuration** via configuration files or commands
- **Dynamic logging level control** without recompilation
- **Performance monitoring** integration
- **Configuration validation** and dependency checking
- **Web-based configuration interface** for ESP32
