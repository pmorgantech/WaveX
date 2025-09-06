#ifndef WAVEX_LINK_CONFIG_H
#define WAVEX_LINK_CONFIG_H

// Include centralized pin configuration
#include "pin_config.h"

// Set to 1 to enable debug logging for inter-MCU packet traffic
#ifndef WAVEX_MCU_LINK_DEBUG
#define WAVEX_MCU_LINK_DEBUG 1
#endif

// SPI link configuration
#define WAVEX_SPI_LINK_ENABLED 1

// Use pin definitions from centralized pin_config.h
#ifdef ESP_PLATFORM
#define ESP_VSPI_HOST          WAVEX_ESP_SPI_HOST
#define PIN_SPI_SCK            WAVEX_ESP_SPI_SCLK
#define PIN_SPI_MOSI           WAVEX_ESP_SPI_MOSI
#define PIN_SPI_MISO           WAVEX_ESP_SPI_MISO
#define PIN_SPI_CS             WAVEX_ESP_SPI_CS
#define PIN_IRQ_DAISY2ESP      WAVEX_ESP_DAISY_IRQ
#define PIN_IRQ_ESP2DAISY      WAVEX_ESP_ATTN_OUT

#define SPI_CLOCK_SPEED_HZ     WAVEX_ESP_SPI_CLK_HZ
#define SPI_QUEUE_SIZE         WAVEX_ESP_SPI_QUEUE_SIZE
#define SPI_DMA_CHANNEL        WAVEX_ESP_SPI_DMA_CH
#else
// Fallback pin definitions for non-ESP builds
#define ESP_VSPI_HOST          SPI3_HOST
#define PIN_SPI_SCK            WAVEX_DAISY_SPI_SCK
#define PIN_SPI_MOSI           WAVEX_DAISY_SPI_MISO
#define PIN_SPI_MISO           WAVEX_DAISY_SPI_MISO
#define PIN_SPI_CS             WAVEX_DAISY_SPI_CS
#define PIN_IRQ_DAISY2ESP      WAVEX_DAISY_IRQ_OUT
#define PIN_IRQ_ESP2DAISY      WAVEX_DAISY_ATTN_IN

#define SPI_CLOCK_SPEED_HZ     (10 * 1000 * 1000)  // 10 MHz
#define SPI_QUEUE_SIZE         4
#define SPI_DMA_CHANNEL        SPI_DMA_CH_AUTO
#endif

// Ring buffer sizes for SPI
#define SPI_RX_RING_SIZE   WAVEX_SPI_RX_RING_SIZE
#define SPI_TX_RING_SIZE   WAVEX_SPI_TX_RING_SIZE
#define SPI_POOL_SIZE      WAVEX_SPI_POOL_SIZE

#endif // WAVEX_LINK_CONFIG_H
