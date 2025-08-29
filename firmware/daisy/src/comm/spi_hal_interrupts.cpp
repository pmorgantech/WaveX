#include "inter_spi_hal.h"
#include "stm32h7xx_hal.h"

#if WAVEX_SPI_LINK_ENABLED

/**
 * @file spi_hal_interrupts.cpp
 * @brief Interrupt handlers for HAL-based SPI slave implementation
 * 
 * These interrupt handlers route STM32 HAL interrupts to our SPI slave implementation.
 * They need to be hooked into the system interrupt vector table.
 */

extern "C" {

// External interrupt for NSS (PA4 = EXTI4)
void EXTI4_IRQHandler(void)
{
    WaveX::Comm::SpiHal_NSS_EXTI_IRQHandler();
}

// Override libDaisy's DMA interrupts using weak attribute mechanism
// Override libDaisy's SPI1 interrupt using weak attribute mechanism
void __attribute__((weak)) SPI1_IRQHandler(void)
{
    // Call our HAL SPI handler
    WaveX::Comm::SpiHal_SPI_IRQHandler();
}

// Override libDaisy's HAL SPI callbacks for our SPI3 instance
void __attribute__((weak)) HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    // Call our custom callback
    WaveX::Comm::SpiHal_TxRxCpltCallback(hspi);
}

void __attribute__((weak)) HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    // Call our custom callback
    WaveX::Comm::SpiHal_ErrorCallback(hspi);
}

} // extern "C"

#endif // WAVEX_SPI_LINK_ENABLED
