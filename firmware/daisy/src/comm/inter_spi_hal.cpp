#include "inter_spi_hal.h"
#include "daisy_seed.h"
#include "sys/system.h"
#include "stm32h7xx_hal.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

#if WAVEX_SPI_LINK_ENABLED

using namespace daisy;

namespace WaveX {
namespace Comm {

// Hardware handles
static SPI_HandleTypeDef hspi1;
static daisy::GPIO irq_pin;  // Daisy IRQ to ESP32 (active low on D13)
static daisy::DaisySeed* g_hw = nullptr;

// Transfer buffers (double-buffered)
static uint8_t txA[HAL_SPI_FRAME_BYTES], txB[HAL_SPI_FRAME_BYTES];
static uint8_t rxA[HAL_SPI_FRAME_BYTES], rxB[HAL_SPI_FRAME_BYTES];
static volatile bool useA = true;
static volatile bool cs_active = false;
static volatile bool transfer_active = false;

// Protocol layer
static pkt_t *rx_pool[WAVEX_SPI_POOL_SIZE];
static pkt_t *rx_queue[WAVEX_SPI_RX_RING_SIZE];
static pkt_t *tx_queue[WAVEX_SPI_TX_RING_SIZE];
static volatile int rx_queue_head = 0, rx_queue_tail = 0;
static volatile int tx_queue_head = 0, tx_queue_tail = 0;
static volatile int rx_pool_count = WAVEX_SPI_POOL_SIZE;

// Statistics
static spi_hal_stats_t stats = {0};

// Minimal TX path: one-packet queue for bring-up
static volatile bool tx_ready = false;
static pkt_t tx_pkt;

static inline void irq_assert_low() {
    if (g_hw) {
        irq_pin.Init(g_hw->GetPin(13), daisy::GPIO::Mode::OUTPUT);
        irq_pin.Write(false);
    }
}
static inline void irq_release_hi() {
    if (g_hw) {
        irq_pin.Init(g_hw->GetPin(13), daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    }
}

// Forward declarations
static void prepare_next_tx(uint8_t *dst);
static void process_rx_frame(const uint8_t *src);
static void configure_gpio(void);
static void configure_spi(void);
static void configure_nss_exti(void);

void SpiHal_Init(daisy::DaisySeed& hw)
{
    g_hw = &hw;
    
    // Initialize packet pool
    for (int i = 0; i < WAVEX_SPI_POOL_SIZE; i++) {
        rx_pool[i] = (pkt_t*)malloc(sizeof(pkt_t));
    }
    
    // Enable clocks
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    
    // Configure hardware
    configure_gpio();
    configure_spi();
    configure_nss_exti();

    // Initialize IRQ line to idle (released)
    irq_release_hi();
    
    // Prepare initial TX buffer
    prepare_next_tx(txA);
    prepare_next_tx(txB);
    
    if (g_hw) {
        g_hw->PrintLine("HAL SPI Slave initialized - waiting for NSS falling edge");
    }
}

static void configure_gpio(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    // Configure SCK pin (PB3, AF5)
    GPIO_InitStruct.Pin = HAL_SPI_SCK_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = HAL_SPI_SCK_AF;
    HAL_GPIO_Init(HAL_SPI_SCK_PORT, &GPIO_InitStruct);
    
    // Configure MOSI pin (PB5, AF5)
    GPIO_InitStruct.Pin = HAL_SPI_MOSI_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = HAL_SPI_MOSI_AF;
    HAL_GPIO_Init(HAL_SPI_MOSI_PORT, &GPIO_InitStruct);
    
    // Configure MISO pin (PB4, AF5) 
    GPIO_InitStruct.Pin = HAL_SPI_MISO_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = HAL_SPI_MISO_AF;
    HAL_GPIO_Init(HAL_SPI_MISO_PORT, &GPIO_InitStruct);
    
    // Configure NSS pin (PA4, AF5) - CRITICAL: Hardware NSS
    GPIO_InitStruct.Pin = HAL_SPI_NSS_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;  // Pull-up for idle high
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = HAL_SPI_NSS_AF;
    HAL_GPIO_Init(HAL_SPI_NSS_PORT, &GPIO_InitStruct);
}

static void configure_spi(void)
{
    hspi1.Instance = HAL_SPI_INSTANCE;
    hspi1.Init.Mode = SPI_MODE_SLAVE;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;        // Mode 0 (CPOL=0)
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;            // Mode 0 (CPHA=0)
    hspi1.Init.NSS = SPI_NSS_HARD_INPUT;              // Hardware NSS
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    
    // H7-specific settings
    hspi1.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_DISABLE;
    hspi1.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    
    HAL_StatusTypeDef result = HAL_SPI_Init(&hspi1);
    if (result != HAL_OK && g_hw) {
        g_hw->PrintLine("HAL_SPI_Init failed: %d", (int)result);
    }
    
    // Enable SPI interrupts
    HAL_NVIC_SetPriority(HAL_SPI_IRQ, 6, 0);
    HAL_NVIC_EnableIRQ(HAL_SPI_IRQ);
}

static void configure_nss_exti(void)
{
    // Configure EXTI for NSS pin (PA4) falling edge
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    // First configure the pin for EXTI (in addition to the AF configuration)
    GPIO_InitStruct.Pin = HAL_SPI_NSS_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;  // Interrupt on falling edge
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(HAL_SPI_NSS_PORT, &GPIO_InitStruct);
    
    // Enable EXTI interrupt
    HAL_NVIC_SetPriority(HAL_SPI_NSS_EXTI, 4, 0);  // High priority
    HAL_NVIC_EnableIRQ(HAL_SPI_NSS_EXTI);
}

static void prepare_next_tx(uint8_t *dst)
{
    if (tx_ready) {
        // Copy full frame from prepared pkt
        memcpy(dst, &tx_pkt, sizeof(tx_pkt));
    } else {
        // Default to zeros if nothing queued
        memset(dst, 0, HAL_SPI_FRAME_BYTES);
    }
}

static void process_rx_frame(const uint8_t *src)
{
    // For bring-up: just count it
    stats.packets_received++;
}

void SpiHal_Service(void)
{
    // Handle NSS falling edge detection
    if (!cs_active) return;
    cs_active = false;
    
    if (transfer_active) {
        // Transfer already in progress, ignore this edge
        return;
    }
    
    // Select buffers (double-buffered)
    uint8_t *tx = useA ? txA : txB;
    uint8_t *rx = useA ? rxA : rxB;
    useA = !useA;
    
    // Prepare next TX data
    prepare_next_tx(tx);
    
    // Start full-duplex interrupt transfer AFTER CS has fallen
    transfer_active = true;
    HAL_StatusTypeDef result = HAL_SPI_TransmitReceive_IT(&hspi1, tx, rx, HAL_SPI_FRAME_BYTES);
    
    if (result != HAL_OK) {
        transfer_active = false;
        if (g_hw) {
            g_hw->PrintLine("HAL_SPI_TransmitReceive_DMA failed: %d", (int)result);
        }
    } else {
        stats.packets_sent++;
    }
}

int SpiHal_Send(uint16_t type, const void* payload, uint16_t len)
{
    if (!g_hw) return 0;
    if (len > sizeof(tx_pkt.payload)) len = sizeof(tx_pkt.payload);
    // Fill packet and zero-pad remainder
    pkt_fill(&tx_pkt, type, payload, len);
    if (len < sizeof(tx_pkt.payload)) {
        memset(tx_pkt.payload + len, 0, sizeof(tx_pkt.payload) - len);
        // Recompute CRC after padding not needed as pkt_crc uses len
    }
    tx_ready = true;
    // Assert IRQ to tell ESP32 data is ready
    irq_assert_low();
    stats.irq_asserts++;
    return 1;
}

int SpiHal_Recv(pkt_t **out)
{
    // TODO: Implement protocol packet reception
    // For now, just return no packets
    *out = nullptr;
    return 0;
}

void SpiHal_Recycle(pkt_t *p, int is_rx)
{
    // TODO: Implement packet recycling
}

bool SpiHal_HasPendingData(void)
{
    // TODO: Check if there are pending packets to send
    return false;
}

void SpiHal_GetStats(spi_hal_stats_t* stats_out)
{
    if (stats_out) {
        *stats_out = stats;
    }
}

void SpiHal_DebugState(void)
{
    if (g_hw) {
        g_hw->PrintLine("HAL SPI Stats: TX=%lu RX=%lu NSS=%lu DMA_TX=%lu DMA_RX=%lu",
            stats.packets_sent, stats.packets_received, stats.nss_interrupts,
            stats.dma_tx_complete, stats.dma_rx_complete);
    }
}

void SpiHal_TestBlocking(void)
{
    if (g_hw) {
        g_hw->PrintLine("HAL SPI blocking test - GPIO levels:");
        g_hw->PrintLine("  NSS: %s", HAL_GPIO_ReadPin(HAL_SPI_NSS_PORT, HAL_SPI_NSS_PIN) ? "HIGH" : "LOW");
        g_hw->PrintLine("  SCK: %s", HAL_GPIO_ReadPin(HAL_SPI_SCK_PORT, HAL_SPI_SCK_PIN) ? "HIGH" : "LOW");
        g_hw->PrintLine("  MOSI: %s", HAL_GPIO_ReadPin(HAL_SPI_MOSI_PORT, HAL_SPI_MOSI_PIN) ? "HIGH" : "LOW");
    }
}

// Interrupt handlers
extern "C" {

void SpiHal_NSS_EXTI_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(HAL_SPI_NSS_PIN);
}

void SpiHal_SPI_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&hspi1);
}

// HAL callback for EXTI
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == HAL_SPI_NSS_PIN) {
        cs_active = true;
        stats.nss_interrupts++;
    }
}

// Custom HAL callback for our SPI3 TX/RX complete (renamed to avoid conflicts)
void SpiHal_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi != &hspi1) return;
    
    transfer_active = false;
    stats.dma_tx_complete++;
    stats.dma_rx_complete++;
    
    // After TX completes, if we had queued data, clear flag and release IRQ
    if (tx_ready) {
        tx_ready = false;
        irq_release_hi();
    }
    
    // Process the received data in the opposite buffer
    uint8_t *done_rx = useA ? rxB : rxA;
    process_rx_frame(done_rx);
}

// Custom HAL callback for our SPI3 errors (renamed to avoid conflicts)
void SpiHal_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi != &hspi1) return;
    
    transfer_active = false;
    stats.dma_errors++;
    
    if (g_hw) {
        g_hw->PrintLine("SPI Error: 0x%08lX", hspi->ErrorCode);
    }
}

} // extern "C"

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED
