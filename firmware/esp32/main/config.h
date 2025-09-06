/**
 * @file config.h
 * @brief ESP32-Specific Configuration
 * 
 * This file contains ESP32-specific configuration macros and includes
 * the shared hardware and logging configurations.
 */

#pragma once

// ============================================================================
// SHARED CONFIGURATION INCLUDES
// ============================================================================

// Include shared hardware component configuration
#include "../../shared/config/hardware_config.h"

// Include shared logging configuration
#include "../../shared/config/logging_config.h"

// ============================================================================
// COMPATIBILITY AND LEGACY SUPPORT
// ============================================================================

// Legacy macro support for existing code
#define WAVEX_ESP32_CONFIG_INCLUDED 1
