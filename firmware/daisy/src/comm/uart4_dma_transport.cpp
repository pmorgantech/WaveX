#include "uart4_dma_transport.h"

#include "memory_sections.h"
#include "stm32h7xx_hal.h"
#include "sys/dma.h"
#include "util/hal_map.h"

namespace WaveX {
namespace Comm {
namespace Uart4Dma {
namespace {

UART_HandleTypeDef s_uart{};
DMA_HandleTypeDef s_rx_dma{};
DMA_HandleTypeDef s_tx_dma{};

uint8_t* s_rx_buffer = nullptr;
size_t s_rx_size = 0;
size_t s_rx_last_pos = 0;
RxCallback s_rx_callback = nullptr;

volatile bool s_receiving = false;
volatile bool s_transmitting = false;
volatile uint8_t s_tx_result = 0;  // 0=pending/none, 1=success, 2=error
volatile uint32_t s_error = 0;

WAVEX_ITCM_CODE void ProcessRxPosition() {
    if (!s_receiving || !s_rx_buffer || s_rx_size == 0 || !s_rx_callback) {
        return;
    }

    size_t pos = s_rx_size - __HAL_DMA_GET_COUNTER(&s_rx_dma);
    if (pos == s_rx_last_pos) {
        return;
    }

    if (pos > s_rx_last_pos) {
        const size_t count = pos - s_rx_last_pos;
        dsy_dma_invalidate_cache_for_buffer(s_rx_buffer + s_rx_last_pos, count);
        s_rx_callback(s_rx_buffer + s_rx_last_pos, count);
    } else {
        const size_t tail = s_rx_size - s_rx_last_pos;
        if (tail > 0) {
            dsy_dma_invalidate_cache_for_buffer(s_rx_buffer + s_rx_last_pos, tail);
            s_rx_callback(s_rx_buffer + s_rx_last_pos, tail);
        }
        if (pos > 0) {
            dsy_dma_invalidate_cache_for_buffer(s_rx_buffer, pos);
            s_rx_callback(s_rx_buffer, pos);
        }
    }
    s_rx_last_pos = (pos == s_rx_size) ? 0 : pos;
}

bool InitDma() {
    s_rx_dma.Instance = DMA1_Stream5;
    s_rx_dma.Init.Request = DMA_REQUEST_UART4_RX;
    s_rx_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    s_rx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    s_rx_dma.Init.MemInc = DMA_MINC_ENABLE;
    s_rx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_rx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    s_rx_dma.Init.Mode = DMA_CIRCULAR;
    s_rx_dma.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    s_rx_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    s_tx_dma.Instance = DMA2_Stream4;
    s_tx_dma.Init.Request = DMA_REQUEST_UART4_TX;
    s_tx_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    s_tx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    s_tx_dma.Init.MemInc = DMA_MINC_ENABLE;
    s_tx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_tx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    s_tx_dma.Init.Mode = DMA_NORMAL;
    s_tx_dma.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    s_tx_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&s_rx_dma) != HAL_OK || HAL_DMA_Init(&s_tx_dma) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&s_uart, hdmarx, s_rx_dma);
    __HAL_LINKDMA(&s_uart, hdmatx, s_tx_dma);
    return true;
}

}  // namespace

bool Init(uint32_t baudrate, daisy::Pin tx_pin, daisy::Pin rx_pin) {
    daisy::GPIOClockEnable(tx_pin);
    daisy::GPIOClockEnable(rx_pin);
    __HAL_RCC_UART4_CLK_ENABLE();

    GPIO_InitTypeDef gpio{};
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF8_UART4;
    gpio.Pin = daisy::GetHALPin(tx_pin);
    HAL_GPIO_Init(daisy::GetHALPort(tx_pin), &gpio);
    gpio.Pin = daisy::GetHALPin(rx_pin);
    HAL_GPIO_Init(daisy::GetHALPort(rx_pin), &gpio);

    s_uart.Instance = UART4;
    s_uart.Init.BaudRate = baudrate;
    s_uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_uart.Init.StopBits = UART_STOPBITS_1;
    s_uart.Init.Parity = UART_PARITY_NONE;
    s_uart.Init.Mode = UART_MODE_TX_RX;
    s_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    s_uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    s_uart.Init.ClockPrescaler = UART_PRESCALER_DIV2;
    s_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&s_uart) != HAL_OK ||
        HAL_UARTEx_SetTxFifoThreshold(&s_uart, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK ||
        HAL_UARTEx_SetRxFifoThreshold(&s_uart, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK ||
        HAL_UARTEx_DisableFifoMode(&s_uart) != HAL_OK || !InitDma()) {
        return false;
    }

    HAL_NVIC_EnableIRQ(UART4_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Stream4_IRQn);
    return true;
}

bool StartReceive(uint8_t* buffer, size_t size, RxCallback callback) {
    if (!buffer || size == 0 || size > UINT16_MAX || !callback || s_receiving) {
        return false;
    }
    s_rx_buffer = buffer;
    s_rx_size = size;
    s_rx_last_pos = 0;
    s_rx_callback = callback;
    dsy_dma_invalidate_cache_for_buffer(buffer, size);
    __HAL_UART_ENABLE_IT(&s_uart, UART_IT_IDLE);
    s_receiving = true;
    if (HAL_UART_Receive_DMA(&s_uart, buffer, static_cast<uint16_t>(size)) != HAL_OK) {
        s_receiving = false;
        __HAL_UART_DISABLE_IT(&s_uart, UART_IT_IDLE);
        s_rx_buffer = nullptr;
        s_rx_size = 0;
        s_rx_callback = nullptr;
        return false;
    }
    return true;
}

bool StopReceive() {
    if (!s_receiving) {
        return true;
    }
    __HAL_UART_DISABLE_IT(&s_uart, UART_IT_IDLE);
    const bool ok = HAL_UART_AbortReceive(&s_uart) == HAL_OK;
    s_receiving = false;
    s_rx_buffer = nullptr;
    s_rx_size = 0;
    s_rx_last_pos = 0;
    s_rx_callback = nullptr;
    return ok;
}

bool IsReceiving() {
    return s_receiving;
}

bool StartTransmit(uint8_t* buffer, size_t size) {
    if (!buffer || size == 0 || size > UINT16_MAX || s_transmitting) {
        return false;
    }
    dsy_dma_clear_cache_for_buffer(buffer, size);
    s_tx_result = 0;
    s_transmitting = true;
    if (HAL_UART_Transmit_DMA(&s_uart, buffer, static_cast<uint16_t>(size)) != HAL_OK) {
        s_transmitting = false;
        s_tx_result = 2;
        return false;
    }
    return true;
}

bool TakeTransmitResult(bool& success) {
    const uint8_t result = s_tx_result;
    if (result == 0) {
        return false;
    }
    s_tx_result = 0;
    success = result == 1;
    return true;
}

bool IsTransmitting() {
    return s_transmitting;
}

uint32_t TakeError() {
    const uint32_t error = s_error | HAL_UART_GetError(&s_uart);
    s_error = 0;
    s_uart.ErrorCode = HAL_UART_ERROR_NONE;
    return error;
}

extern "C" void UART4_IRQHandler() {
    if (__HAL_UART_GET_FLAG(&s_uart, UART_FLAG_IDLE) &&
        __HAL_UART_GET_IT_SOURCE(&s_uart, UART_IT_IDLE)) {
        __HAL_UART_CLEAR_IDLEFLAG(&s_uart);
        ProcessRxPosition();
    }
    HAL_UART_IRQHandler(&s_uart);
}

extern "C" void DMA1_Stream5_IRQHandler() {
    HAL_DMA_IRQHandler(&s_rx_dma);
}

extern "C" void DMA2_Stream4_IRQHandler() {
    HAL_DMA_IRQHandler(&s_tx_dma);
}

extern "C" void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef* handle) {
    if (handle == &s_uart) {
        ProcessRxPosition();
    }
}

extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* handle) {
    if (handle == &s_uart) {
        ProcessRxPosition();
    }
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* handle) {
    if (handle == &s_uart) {
        s_transmitting = false;
        s_tx_result = 1;
    }
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* handle) {
    if (handle == &s_uart) {
        s_error |= HAL_UART_GetError(handle);
        if (s_transmitting) {
            s_transmitting = false;
            s_tx_result = 2;
        }
        if (handle->RxState == HAL_UART_STATE_READY) {
            s_receiving = false;
        }
    }
}

// System::Init calls every libDaisy peripheral's global initializer. The
// WaveX target omits libDaisy's UART translation unit, so provide the UART
// initializer it expects; all state above is initialized explicitly by Init.
extern "C" void dsy_uart_global_init() {}

}  // namespace Uart4Dma
}  // namespace Comm
}  // namespace WaveX
