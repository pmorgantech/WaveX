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

// SPI link configuration.
// 0 = SPI link compiled out. Decision recorded 2026-07-05 (architecture.md
// §4.4): UART is the transport of record and carries all inter-MCU traffic;
// re-enabling SPI requires bench re-validation (roadmap Phase 1 item 6).
#define WAVEX_SPI_LINK_ENABLED 0

#ifndef WAVEX_SPI_DMA_ENABLED
#define WAVEX_SPI_DMA_ENABLED 0  // Disabled - not using SPI currently, save DMA memory
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

#define SPI_CLOCK_SPEED_HZ WAVEX_ESP_SPI_CLK_HZ
#define SPI_QUEUE_SIZE WAVEX_ESP_SPI_QUEUE_SIZE
#define SPI_DMA_CHANNEL WAVEX_ESP_SPI_DMA_CH
#else
// Fallback pin definitions for non-ESP builds
#define ESP_VSPI_HOST SPI3_HOST
#define PIN_SPI_SCK WAVEX_DAISY_SPI_SCK
#define PIN_SPI_MOSI WAVEX_DAISY_SPI_MISO
#define PIN_SPI_MISO WAVEX_DAISY_SPI_MISO
#define PIN_SPI_CS WAVEX_DAISY_SPI_CS
#define PIN_IRQ_DAISY2ESP WAVEX_DAISY_IRQ_OUT
#define PIN_IRQ_ESP2DAISY WAVEX_DAISY_ATTN_IN

#define SPI_CLOCK_SPEED_HZ 10000000
#define SPI_QUEUE_SIZE 4
#define SPI_DMA_CHANNEL SPI_DMA_CH_AUTO
#endif

// Ring buffer sizes for SPI
#define SPI_RX_RING_SIZE WAVEX_SPI_RX_RING_SIZE
#define SPI_TX_RING_SIZE WAVEX_SPI_TX_RING_SIZE
#define SPI_POOL_SIZE WAVEX_SPI_POOL_SIZE

// HD Protocol Commands (Espressif SPI Slave HD Protocol)
#define WAVEX_HD_WRDMA 0x03    // Master→slave data transfer command
#define WAVEX_HD_RDDMA 0x04    // Slave→master data transfer command
#define WAVEX_HD_WR_DONE 0x07  // Terminate write segment command
#define WAVEX_HD_CMD8 0x08     // Terminate read segment command
#define WAVEX_HD_ADDR 0x00     // Default address byte for HD protocol

#endif  // WAVEX_LINK_CONFIG_H
