#pragma once

// Daisy <-> ESP32 UART baud rate
#ifndef INTER_MCU_UART_BAUD_RATE
#define INTER_MCU_UART_BAUD_RATE 460800
#endif

// Set to 1 to completely disable UART init and polling for isolation testing
#ifndef WAVEX_DEBUG_DISABLE_UART
#define WAVEX_DEBUG_DISABLE_UART 0
#endif

// Set to 1 to enable UART RX via IRQ; leave 0 for polling-only mode
#ifndef WAVEX_UART_RX_IRQ_MODE
#define WAVEX_UART_RX_IRQ_MODE 1
#endif

// Set to 1 to enable the audio engine, 0 to disable
#ifndef WAVEX_AUDIO_ENGINE_ENABLED
#define WAVEX_AUDIO_ENGINE_ENABLED 1
#endif

// The WAVEX_UART_DEBUG_LOG has been replaced by the protocol-agnostic
// WAVEX_MCU_LINK_DEBUG in shared/config/link_config.h
// #ifndef WAVEX_UART_DEBUG_LOG
// #define WAVEX_UART_DEBUG_LOG 0
// #endif


