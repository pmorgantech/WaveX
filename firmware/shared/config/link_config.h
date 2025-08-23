#ifndef WAVEX_LINK_CONFIG_H
#define WAVEX_LINK_CONFIG_H

// Link selection configuration
// Set to 1 to use SPI link, 0 to use UART link
#ifndef WAVEX_USE_SPI_LINK
#define WAVEX_USE_SPI_LINK 0
#endif

#ifndef WAVEX_USE_UART_LINK
#define WAVEX_USE_UART_LINK (!WAVEX_USE_SPI_LINK)
#endif

// SPI link configuration
#if WAVEX_USE_SPI_LINK
    #define WAVEX_SPI_LINK_ENABLED 1
    #define WAVEX_UART_LINK_ENABLED 
#else
    #define WAVEX_SPI_LINK_ENABLED 0
    #define WAVEX_UART_LINK_ENABLED 1
#endif

// Pin configurations for ESP32-S3
#if WAVEX_SPI_LINK_ENABLED
    #define ESP_VSPI_HOST     SPI3_HOST
    #define PIN_SPI_SCK       36
    #define PIN_SPI_MOSI      35
    #define PIN_SPI_MISO      37
    #define PIN_SPI_CS        39
    #define PIN_IRQ_DAISY2ESP 34
    #define PIN_IRQ_ESP2DAISY 33
    
    #define SPI_CLOCK_SPEED_HZ (10 * 1000 * 1000)  // 10 MHz
    #define SPI_QUEUE_SIZE     4
    #define SPI_DMA_CHANNEL    SPI_DMA_CH_AUTO
#endif

// UART link configuration (kept for fallback)
#if WAVEX_UART_LINK_ENABLED
    #define INTER_MCU_UART_BAUD_RATE 460800
    #define INTER_MCU_UART_BUFFER_SIZE 512
#endif

// Ring buffer sizes for SPI
#if WAVEX_SPI_LINK_ENABLED
    #define SPI_RX_RING_SIZE   32
    #define SPI_TX_RING_SIZE   32
    #define SPI_POOL_SIZE      16
#endif

#endif // WAVEX_LINK_CONFIG_H
