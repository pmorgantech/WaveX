#ifndef WAVEX_SPI_LINK_H
#define WAVEX_SPI_LINK_H

#include <stdint.h>
#include "link_config.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

// SPI link configuration
// WAVEX_SPI_LINK_ENABLED is defined in shared/config/link_config.h

#if WAVEX_SPI_LINK_ENABLED

// Pin map for ESP32-S3 SPI master
#define ESP_VSPI_HOST     SPI3_HOST
#define PIN_SPI_SCK       36
#define PIN_SPI_MOSI      35
#define PIN_SPI_MISO      37
#define PIN_SPI_CS        39
#define PIN_IRQ_DAISY2ESP 34  // Daisy asserts when it has TX data ready
#define PIN_IRQ_ESP2DAISY 33  // ESP asserts to request attention (optional)

// SPI configuration
#define SPI_CLOCK_SPEED_HZ (10 * 1000 * 1000)  // 10 MHz
#define SPI_QUEUE_SIZE     4
#define SPI_DMA_CHANNEL    SPI_DMA_CH_AUTO

// Ring buffer sizes
#define SPI_RX_RING_SIZE   32
#define SPI_TX_RING_SIZE   32
#define SPI_POOL_SIZE      16

// Function declarations
esp_err_t spi_link_init(void);
esp_err_t spi_link_start(void);
int spi_link_send(uint16_t type, const void* payload, uint16_t len);
int spi_link_recv(void** out);
void spi_link_recycle(void* p, int is_rx);

// Get link statistics
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t crc_errors;
    uint32_t irq_count;
    uint32_t last_activity_ms;
} spi_link_stats_t;

void spi_link_get_stats(spi_link_stats_t* stats);

#endif // WAVEX_SPI_LINK_ENABLED

#endif // WAVEX_SPI_LINK_H
