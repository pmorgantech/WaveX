#ifndef WAVEX_LINK_CONFIG_H
#define WAVEX_LINK_CONFIG_H

// Include centralized pin configuration
#include "pin_config.h"

// Set to 1 for link-level debug logging (connection/dispatch events)
#ifndef WAVEX_MCU_LINK_DEBUG
#define WAVEX_MCU_LINK_DEBUG 0
#endif

// Set to 1 for per-packet trace/dump logging - far noisier than
// WAVEX_MCU_LINK_DEBUG, and each dump is a blocking log write
#ifndef WAVEX_MCU_LINK_PACKET_DEBUG
#define WAVEX_MCU_LINK_PACKET_DEBUG 0
#endif

// Shared build-time transport selector. Rebuild and flash BOTH MCUs after
// changing it: 0 = UART (default), 1 = experimental SPI. No runtime fallback.
#ifndef WAVEX_SPI_LINK_ENABLED
#define WAVEX_SPI_LINK_ENABLED 0
#endif
#if WAVEX_SPI_LINK_ENABLED != 0 && WAVEX_SPI_LINK_ENABLED != 1
#error "WAVEX_SPI_LINK_ENABLED must be 0 (UART) or 1 (SPI)"
#endif
#ifndef WAVEX_SPI_DMA_ENABLED
#define WAVEX_SPI_DMA_ENABLED WAVEX_SPI_LINK_ENABLED
#endif
#if WAVEX_SPI_DMA_ENABLED != WAVEX_SPI_LINK_ENABLED
#error "SPI DMA must follow the selected transport"
#endif
#if WAVEX_SPI_LINK_ENABLED
#define WAVEX_MCU_LINK_NAME "SPI"
#else
#define WAVEX_MCU_LINK_NAME "UART"
#endif

// Use pin definitions from centralized pin_config.h
#ifdef ESP_PLATFORM
#define ESP_VSPI_HOST WAVEX_ESP_SPI_HOST
#define PIN_SPI_SCK WAVEX_ESP_SPI_SCLK
#define PIN_SPI_MOSI WAVEX_ESP_SPI_MOSI
#define PIN_SPI_MISO WAVEX_ESP_SPI_MISO
#define PIN_SPI_CS WAVEX_ESP_SPI_CS
#define PIN_IRQ_DAISY2ESP WAVEX_ESP_DAISY_IRQ
#define PIN_IRQ_ESP2DAISY WAVEX_ESP_ATTN_OUT

#define SPI_QUEUE_SIZE WAVEX_ESP_SPI_QUEUE_SIZE
#define SPI_DMA_CHANNEL WAVEX_ESP_SPI_DMA_CH
#else
// Fallback pin definitions for non-ESP builds
#define ESP_VSPI_HOST SPI3_HOST
#define PIN_SPI_SCK WAVEX_DAISY_SPI_SCK
#define PIN_SPI_MOSI WAVEX_DAISY_SPI_MOSI
#define PIN_SPI_MISO WAVEX_DAISY_SPI_MISO
#define PIN_SPI_CS WAVEX_DAISY_SPI_CS
#define PIN_IRQ_DAISY2ESP WAVEX_DAISY_IRQ_OUT
#define PIN_IRQ_ESP2DAISY WAVEX_DAISY_ATTN_IN

#define SPI_QUEUE_SIZE 4
#define SPI_DMA_CHANNEL SPI_DMA_CH_AUTO
#endif

// Ring buffer sizes for SPI
#define SPI_RX_RING_SIZE WAVEX_SPI_RX_RING_SIZE
#define SPI_TX_RING_SIZE WAVEX_SPI_TX_RING_SIZE

// HD Protocol Commands (Espressif SPI Slave HD Protocol)
#define WAVEX_HD_WRDMA 0x03    // Master→slave data transfer command
#define WAVEX_HD_RDDMA 0x04    // Slave→master data transfer command
#define WAVEX_HD_WR_DONE 0x07  // Terminate write segment command
#define WAVEX_HD_CMD8 0x08     // Terminate read segment command
#define WAVEX_HD_ADDR 0x00     // Default address byte for HD protocol

#endif  // WAVEX_LINK_CONFIG_H
