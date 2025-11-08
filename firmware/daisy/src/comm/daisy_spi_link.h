#ifndef WAVEX_INTER_SPI_H
#define WAVEX_INTER_SPI_H

#include <stdint.h>
#include <cstdint>
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

// ============================================================================
// Statistics Structure (must be declared before functions that use it)
// ============================================================================

typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t crc_errors;
    uint32_t rx_q_overflows;
    uint32_t irq_asserts;
    uint32_t last_activity_ms;
} spi_link_stats_t;

// ============================================================================
// Core SPI Communication Functions
// ============================================================================

void Spi_Init(daisy::DaisySeed& hw, daisy::SpiHandle* hspi);
bool Spi_SendPreCreatedPacket(const uint8_t* packet_data, size_t packet_size);

// ============================================================================
// Message Processing Functions
// ============================================================================

void ProcessQueuedSpiMessage(void);

// ============================================================================
// Utility and Debug Functions
// ============================================================================

void Spi_CheckTimeout(void);
void Spi_GetStats(spi_link_stats_t* stats);
void Spi_DebugState(void);

// Poll the ATTN level and trigger a receive if asserted (fallback when EXTI edge is missed)
bool Spi_PollAttnLevel(void);

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED

#endif // WAVEX_INTER_SPI_H
