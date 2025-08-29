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
// All pin definitions are now centralized in shared/config/pin_config.h

#if WAVEX_SPI_LINK_ENABLED

// Pin definitions are now sourced from centralized pin_config.h via link_config.h
// No duplicate definitions needed here

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
    uint32_t rx_pool_empty;
    uint32_t last_activity_ms;
} spi_link_stats_t;

void spi_link_get_stats(spi_link_stats_t* stats);

// Set packet processing callback for received packets
void spi_link_set_packet_callback(void (*callback)(const uint8_t* data, size_t length));

#endif // WAVEX_SPI_LINK_ENABLED

#endif // WAVEX_SPI_LINK_H
