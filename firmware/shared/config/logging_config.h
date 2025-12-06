/**
 * @file logging_config.h
 * @brief WaveX Logging Configuration
 *
 * This file defines logging macros for all hardware components
 * to allow selective debug output control at compile time.
 *
 * Each component has its own logging macro that can be set to 0
 * to completely compile out all debug logging for that component.
 * This allows for production builds with minimal debug overhead
 * and easy troubleshooting of specific components.
 */

#pragma once

#include <cstdarg>
#include <cstdio>

// ============================================================================
// GLOBAL LOGGING CONTROL
// ============================================================================

// Master switch for all debug logging (set to 0 to disable all debug output)
#ifndef WAVEX_DEBUG_LOGGING_ENABLED
#define WAVEX_DEBUG_LOGGING_ENABLED 1
#endif

// ============================================================================
// COMPONENT-SPECIFIC LOGGING MACROS
// ============================================================================

// Meter Data Logging (ESP32 only)
#ifndef WAVEX_LOG_METER_DATA
#define WAVEX_LOG_METER_DATA 0
#endif

// Inter-MCU Communication Link Logging
#ifndef WAVEX_LOG_INTER_MCU_LINK
#define WAVEX_LOG_INTER_MCU_LINK WAVEX_DEBUG_LOGGING_ENABLED
#endif

// Audio Engine Logging (Daisy only)
#ifndef WAVEX_LOG_AUDIO_ENGINE
#define WAVEX_LOG_AUDIO_ENGINE WAVEX_DEBUG_LOGGING_ENABLED
#endif

// DAC CV Outputs Logging (Daisy only)
#ifndef WAVEX_LOG_DAC_CV_OUTPUTS
#define WAVEX_LOG_DAC_CV_OUTPUTS WAVEX_DEBUG_LOGGING_ENABLED
#endif

// Encoder Logging (ESP32 only)
#ifndef WAVEX_LOG_ENCODER
#define WAVEX_LOG_ENCODER WAVEX_DEBUG_LOGGING_ENABLED
#endif

// 4067 Mux Logging (ESP32 only)
#ifndef WAVEX_LOG_4067_MUX
#define WAVEX_LOG_4067_MUX WAVEX_DEBUG_LOGGING_ENABLED
#endif

// TCA8418 Button Matrix Logging (ESP32 only)
#ifndef WAVEX_LOG_TCA8418_BUTTON_MATRIX
#define WAVEX_LOG_TCA8418_BUTTON_MATRIX WAVEX_DEBUG_LOGGING_ENABLED
#endif

// LCD Display Logging (ESP32 only)
#ifndef WAVEX_LOG_LCD_DISPLAY
#define WAVEX_LOG_LCD_DISPLAY WAVEX_DEBUG_LOGGING_ENABLED
#endif

// USB MIDI Logging (ESP32 only)
#ifndef WAVEX_LOG_USB_MIDI
#define WAVEX_LOG_USB_MIDI WAVEX_DEBUG_LOGGING_ENABLED
#endif

// Daisy SPI Outbound Packet Logging (Daisy only)
#ifndef WAVEX_LOG_DAISY_OUTBOUND_SPI
#define WAVEX_LOG_DAISY_OUTBOUND_SPI 0
#endif

// Daisy SPI Inbound Packet Logging (Daisy only)
#ifndef WAVEX_LOG_DAISY_INBOUND_SPI
#define WAVEX_LOG_DAISY_INBOUND_SPI 0
#endif

// Daisy SPI Packet Summary Logging (Daisy only)
#ifndef WAVEX_LOG_DAISY_SPI_PACKET
#define WAVEX_LOG_DAISY_SPI_PACKET WAVEX_DEBUG_LOGGING_ENABLED
#endif

// Daisy SPI Message Decoding Logging (Daisy only)
#ifndef WAVEX_LOG_DAISY_SPI_MESSAGE
#define WAVEX_LOG_DAISY_SPI_MESSAGE WAVEX_DEBUG_LOGGING_ENABLED
#endif

// ESP32_INTER_SPI component logging (for token pasting with ESP32_INTER_SPI)
#ifndef WAVEX_LOG_ESP32_INTER_SPI
#define WAVEX_LOG_ESP32_INTER_SPI 0
#endif

// ESP32 SPI component logging (for token pasting)

// Storage/Filesystem Logging (Daisy only)
#ifndef WAVEX_LOG_STORAGE
#define WAVEX_LOG_STORAGE WAVEX_DEBUG_LOGGING_ENABLED
#endif

// ============================================================================
// LOGGING MACRO DEFINITIONS
// ============================================================================

// Generic logging macro that can be used across platforms
#if WAVEX_DEBUG_LOGGING_ENABLED
#define WAVEX_LOG(component, format, ...)                                 \
    do {                                                                  \
        if (WAVEX_LOG_##component) {                                      \
            printf("[WAVEX-%s] " format "\n", #component, ##__VA_ARGS__); \
        }                                                                 \
    } while (0)

#define WAVEX_LOG_RAW(component, format, ...) \
    do {                                      \
        if (WAVEX_LOG_##component) {          \
            printf(format, ##__VA_ARGS__);    \
        }                                     \
    } while (0)
#else
#define WAVEX_LOG(component, format, ...) ((void)0)
#define WAVEX_LOG_RAW(component, format, ...) ((void)0)
#endif

// ============================================================================
// PLATFORM-SPECIFIC LOGGING MACROS
// ============================================================================

#ifdef ESP_PLATFORM
// ESP32-specific logging using ESP_LOG macros
#include "esp_log.h"

#define WAVEX_LOG_ESP(component, level, format, ...)                     \
    do {                                                                 \
        if (WAVEX_LOG_##component) {                                     \
            ESP_LOG_##level("WAVEX-" #component, format, ##__VA_ARGS__); \
        }                                                                \
    } while (0)

#define WAVEX_LOG_ESP_INFO(component, format, ...) \
    WAVEX_LOG_ESP(component, INFO, format, ##__VA_ARGS__)

#define WAVEX_LOG_ESP_WARN(component, format, ...) \
    WAVEX_LOG_ESP(component, WARN, format, ##__VA_ARGS__)

#define WAVEX_LOG_ESP_ERROR(component, format, ...) \
    WAVEX_LOG_ESP(component, ERROR, format, ##__VA_ARGS__)

#define WAVEX_LOG_ESP_DEBUG(component, format, ...) \
    WAVEX_LOG_ESP(component, DEBUG, format, ##__VA_ARGS__)

#define WAVEX_LOG_ESP_VERBOSE(component, format, ...) \
    WAVEX_LOG_ESP(component, VERBOSE, format, ##__VA_ARGS__)

// ESP32 inter-SPI debugging logging
#define WAVEX_LOG_ESP32_SPI(component, format, ...)               \
    do {                                                          \
        if (WAVEX_LOG_##component) {                              \
            ESP_LOGI("WAVEX-" #component, format, ##__VA_ARGS__); \
        }                                                         \
    } while (0)

#else
// Daisy-specific logging routed through a helper that can use s_hw->PrintLine when available.
// Implementations are in firmware/daisy/src/comm/daisy_logging.cpp
void wavex_daisy_log(const char* format, ...);
void wavex_daisy_log_raw(const char* format, ...);

// Helper wrappers to keep per-component log gating while using the raw logging helpers directly.
inline void wavex_daisy_log_if(bool enabled,
                               const char* component,
                               const char* format,
                               ...) {
    if (!enabled) {
        return;
    }

    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    wavex_daisy_log("[WAVEX-%s] %s", component, buf);
}

inline void wavex_daisy_log_raw_if(bool enabled, const char* format, ...) {
    if (!enabled) {
        return;
    }

    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    wavex_daisy_log_raw("%s", buf);
}

#define WAVEX_LOG_DAISY(component, format, ...)        \
    do {                                              \
        if (WAVEX_LOG_##component) {                  \
            wavex_daisy_log("[WAVEX-%s] " format,     \
                            #component,               \
                            ##__VA_ARGS__);           \
        }                                             \
    } while (0)

#define WAVEX_LOG_DAISY_RAW(component, format, ...) \
    do {                                           \
        if (WAVEX_LOG_##component) {               \
            wavex_daisy_log_raw(format, ##__VA_ARGS__); \
        }                                          \
    } while (0)
#endif

// ============================================================================
// CONVENIENCE MACROS FOR COMMON LOGGING PATTERNS
// ============================================================================

// Component initialization logging
#define WAVEX_LOG_INIT(component, format, ...) \
    WAVEX_LOG(component, "Initializing: " format, ##__VA_ARGS__)

#define WAVEX_LOG_INIT_SUCCESS(component, format, ...) \
    WAVEX_LOG(component, "Initialized successfully: " format, ##__VA_ARGS__)

#define WAVEX_LOG_INIT_FAILED(component, format, ...) \
    WAVEX_LOG(component, "Initialization failed: " format, ##__VA_ARGS__)

// Component state change logging
#define WAVEX_LOG_STATE_CHANGE(component, format, ...) \
    WAVEX_LOG(component, "State change: " format, ##__VA_ARGS__)

// Component error logging
#define WAVEX_LOG_ERROR(component, format, ...) \
    WAVEX_LOG(component, "ERROR: " format, ##__VA_ARGS__)

// Component warning logging
#define WAVEX_LOG_WARN(component, format, ...) \
    WAVEX_LOG(component, "WARNING: " format, ##__VA_ARGS__)

// Component debug logging
#define WAVEX_LOG_DEBUG(component, format, ...) \
    WAVEX_LOG(component, "DEBUG: " format, ##__VA_ARGS__)

// ============================================================================
// SPI-SPECIFIC LOGGING MACROS
// ============================================================================

// Daisy outbound SPI packet logging
#define WAVEX_LOG_DAISY_OUTBOUND(component, format, ...) \
    WAVEX_LOG_DAISY(component, format, ##__VA_ARGS__)

// Daisy inbound SPI packet logging
#define WAVEX_LOG_DAISY_INBOUND(component, format, ...) \
    WAVEX_LOG_DAISY(component, format, ##__VA_ARGS__)

// Daisy SPI packet summary logging
#define WAVEX_LOG_DAISY_PACKET(component, format, ...) \
    WAVEX_LOG_DAISY(component, format, ##__VA_ARGS__)

// Daisy SPI message decoding logging
#define WAVEX_LOG_DAISY_MESSAGE(component, format, ...) \
    WAVEX_LOG_DAISY(component, format, ##__VA_ARGS__)

// ============================================================================
// CONDITIONAL COMPILATION HELPERS
// ============================================================================

// Macro to check if a component is enabled and logging is enabled
#define WAVEX_COMPONENT_LOGGING_ENABLED(component) \
    (WAVEX_LOG_##component && WAVEX_DEBUG_LOGGING_ENABLED)

// Macro to conditionally execute code only when logging is enabled for a component
#define WAVEX_IF_LOGGING(component, code)                 \
    do {                                                  \
        if (WAVEX_COMPONENT_LOGGING_ENABLED(component)) { \
            code;                                         \
        }                                                 \
    } while (0)
