#ifndef WAVEX_INTER_SPI_H
#define WAVEX_INTER_SPI_H

#include <stdint.h>
#include "daisy_seed.h"
#include "../../shared/spi_protocol/spi_protocol.h"
#include "config/link_config.h"

// SPI link configuration is now handled by the shared link_config.h

#if WAVEX_SPI_LINK_ENABLED

// Forward declare the libDaisy SpiHandle
namespace daisy { class SpiHandle; }

namespace WaveX {
namespace Comm {

// Ring buffer sizes are now defined in link_config.h

// Function declarations
void Spi_Init(daisy::DaisySeed& hw, daisy::SpiHandle* hspi);
int Spi_Send(uint16_t type, const void* payload, uint16_t len);
int Spi_Recv(pkt_t **out);
void Spi_Recycle(pkt_t *p, int is_rx);
bool Spi_HasPendingData(void);

// Get link statistics
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t crc_errors;
    uint32_t rx_q_overflows;
    uint32_t irq_asserts;
    uint32_t last_activity_ms;
} spi_link_stats_t;

void Spi_GetStats(spi_link_stats_t* stats);
void Spi_DebugState(void);
void Spi_ValidateBuffers(void);
void Spi_ForceReset(void);

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED

#endif // WAVEX_INTER_SPI_H
