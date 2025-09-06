#pragma once

// ============================================================================
// SHARED CONFIGURATION INCLUDES
// ============================================================================

// Include shared hardware component configuration
#include "../shared/config/hardware_config.h"

// Include shared logging configuration
#include "../shared/config/logging_config.h"

// ============================================================================
// DAISY-SPECIFIC CONFIGURATION
// ============================================================================

// UART support removed - using SPI only

// ============================================================================
// LEGACY MACRO SUPPORT (for backward compatibility)
// ============================================================================

// The WAVEX_UART_DEBUG_LOG has been replaced by the protocol-agnostic
// WAVEX_MCU_LINK_DEBUG in shared/config/link_config.h
// #ifndef WAVEX_UART_DEBUG_LOG
// #define WAVEX_UART_DEBUG_LOG 0
// #endif

// Legacy audio engine macro (now handled by shared hardware config)
// #ifndef WAVEX_AUDIO_ENGINE_ENABLED
// #define WAVEX_AUDIO_ENGINE_ENABLED 1
// #endif


