#ifndef WAVEX_INTER_SPI_HAL_H
#define WAVEX_INTER_SPI_HAL_H

#include <stdint.h>
#include <stdbool.h>
#define DAISY_PLATFORM 1
#include "daisy_seed.h"
#include "stm32h7xx_hal.h"
#include "../shared/config/pin_config.h"
#include "spi_protocol/spi_protocol.h"
#include "config/link_config.h"

/**
 * @file inter_spi_hal.h
 * @brief Raw STM32 HAL SPI Slave Implementation for Inter-MCU Communication
 * 
 * This module implements direct STM32 HAL SPI slave functionality as recommended
 * in the troubleshooting documentation to achieve reliable hardware NSS operation
 * that libDaisy's SPI abstractions cannot provide in slave mode.
 * 
 * Key Features:
 * - Hardware NSS (CS) detection via EXTI interrupt
 * - DMA-based transfers armed on NSS falling edge
 * - Double-buffered RX/TX for continuous operation
 * - Direct GPIO and SPI peripheral configuration
 */

#if WAVEX_SPI_LINK_ENABLED

// Forward declarations
namespace daisy { class DaisySeed; }

namespace WaveX {
namespace Comm {

// Frame size for SPI transactions (full pkt_t frame: 2 len + 2 type + 240 payload + 2 crc)
#define HAL_SPI_FRAME_BYTES  246

// SPI instance configuration for Daisy Seed (use SPI1 to match wiring D8/D9/D10/D7)
#define HAL_SPI_INSTANCE     SPI1
#define HAL_SPI_IRQ          SPI1_IRQn

// We will use interrupt-driven transfers (no DMA) to avoid DMAMUX conflicts

// GPIO pin definitions for SPI1 on Daisy Seed
// SCK: PB3 (AF5), MOSI: PB5 (AF5), MISO: PB4 (AF5), NSS: PA4 (AF5)
#define HAL_SPI_SCK_PORT     GPIOB
#define HAL_SPI_SCK_PIN      GPIO_PIN_3
#define HAL_SPI_SCK_AF       GPIO_AF5_SPI1

#define HAL_SPI_MOSI_PORT    GPIOB
#define HAL_SPI_MOSI_PIN     GPIO_PIN_5
#define HAL_SPI_MOSI_AF      GPIO_AF5_SPI1

#define HAL_SPI_MISO_PORT    GPIOB
#define HAL_SPI_MISO_PIN     GPIO_PIN_4
#define HAL_SPI_MISO_AF      GPIO_AF5_SPI1

#define HAL_SPI_NSS_PORT     GPIOA
#define HAL_SPI_NSS_PIN      GPIO_PIN_4
#define HAL_SPI_NSS_AF       GPIO_AF5_SPI1
#define HAL_SPI_NSS_EXTI     EXTI4_IRQn

// Function declarations
void SpiHal_Init(daisy::DaisySeed& hw);
void SpiHal_Service(void);  // Call from main loop
int SpiHal_Send(uint16_t type, const void* payload, uint16_t len);
int SpiHal_Recv(pkt_t **out);
void SpiHal_Recycle(pkt_t *p, int is_rx);
bool SpiHal_HasPendingData(void);

// Get link statistics
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t crc_errors;
    uint32_t rx_q_overflows;
    uint32_t irq_asserts;
    uint32_t last_activity_ms;
    uint32_t nss_interrupts;
    uint32_t dma_tx_complete;
    uint32_t dma_rx_complete;
    uint32_t dma_errors;
} spi_hal_stats_t;

void SpiHal_GetStats(spi_hal_stats_t* stats);
void SpiHal_DebugState(void);
void SpiHal_TestBlocking(void);  // Minimal blocking test

// Interrupt handlers (called from stm32h7xx_it.c)
extern "C" {
    void SpiHal_NSS_EXTI_IRQHandler(void);
    void SpiHal_DMA_TX_IRQHandler(void);
    void SpiHal_DMA_RX_IRQHandler(void);
    void SpiHal_SPI_IRQHandler(void);
    
    // Custom HAL callbacks (avoid conflicts with libDaisy)
    void SpiHal_TxRxCpltCallback(SPI_HandleTypeDef *hspi);
    void SpiHal_ErrorCallback(SPI_HandleTypeDef *hspi);
}

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED

#endif // WAVEX_INTER_SPI_HAL_H
