#ifndef WAVEX_INTER_SPI_H
#define WAVEX_INTER_SPI_H

#include "daisy_seed.h"
#include "spi_protocol/spi_protocol.h"
#include "config/link_config.h"

// SPI link configuration is now handled by the shared link_config.h

#if WAVEX_SPI_LINK_ENABLED

namespace WaveX {
namespace Comm {

// Ring buffer sizes (already defined in link_config.h, but kept here for clarity)
#define SPI_RX_RING_SIZE   32
#define SPI_TX_RING_SIZE   32
#define SPI_POOL_SIZE      16

// Function declarations
void Spi_Init(daisy::DaisySeed& hw);
int Spi_Send(uint16_t type, const void* payload, uint16_t len);
int Spi_Recv(pkt_t **out);
void Spi_Recycle(pkt_t *p, int is_rx);
bool Spi_HasPendingData(void);

// Get link statistics
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t crc_errors;
    uint32_t irq_asserts;
    uint32_t last_activity_ms;
} spi_link_stats_t;

void Spi_GetStats(spi_link_stats_t* stats);

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED

#endif // WAVEX_INTER_SPI_H
