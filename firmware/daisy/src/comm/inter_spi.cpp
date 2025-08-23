#include "inter_spi.h"

#if WAVEX_SPI_LINK_ENABLED

#include "stm32h7xx_hal.h"
#include "daisy_seed.h"
#include <string.h>

// Choose an SPI peripheral that's free on your board
// Note: This is a placeholder - actual SPI handle should be configured in CubeMX
// extern SPI_HandleTypeDef hspi2; // for example

// IRQ control functions - using actual GPIO definitions
static inline void irq_assert()  { 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET); 
}
static inline void irq_release() { 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);   
}

// ---- Queues and pools -------------------------------------------------------
static ring_t rx_q, tx_q;
static void *rx_backing[SPI_RX_RING_SIZE], *tx_backing[SPI_TX_RING_SIZE];
static pkt_t rx_pool[SPI_POOL_SIZE], tx_pool[SPI_POOL_SIZE];
static ring_t free_rx, free_tx;
static void*  backing_frx[SPI_POOL_SIZE], *backing_ftx[SPI_POOL_SIZE];

// Statistics
static WaveX::Comm::spi_link_stats_t s_stats = {};

static void init_pools()
{
    ring_init(&free_rx, backing_frx, SPI_POOL_SIZE);
    ring_init(&free_tx, backing_ftx, SPI_POOL_SIZE);
    ring_init(&rx_q, rx_backing, SPI_RX_RING_SIZE);
    ring_init(&tx_q, tx_backing, SPI_TX_RING_SIZE);
    for (int i=0; i<SPI_POOL_SIZE; i++) { 
        ring_push(&free_rx, &rx_pool[i]); 
        ring_push(&free_tx, &tx_pool[i]); 
    }
}

// ---- Ping-pong DMA buffers --------------------------------------------------
static pkt_t rxA, rxB, txA, txB;
static volatile int using_A = 1;

static void prime_next_buffers()
{
    pkt_t *rx = using_A ? &rxA : &rxB;
    pkt_t *tx = using_A ? &txA : &txB;

    // If we have an app packet queued, copy into tx; else send a 0-len ping
    pkt_t *app_tx = (pkt_t*)ring_pop(&tx_q);
    if (app_tx) { 
        *tx = *app_tx; 
        ring_push(&free_tx, app_tx); 
    }
    else { 
        pkt_fill(tx, 0x0000, nullptr, 0); 
    }

    // Start duplex DMA for a full "max frame": header first, then tail (payload+crc)
    // Note: This requires actual SPI handle configuration
    // HAL_SPI_TransmitReceive_DMA(&hspi2, (uint8_t*)&tx->h, (uint8_t*)&rx->h, sizeof(pkt_hdr_t));
}

// SPI completion callback - renamed to avoid conflicts with libDaisy
extern "C" void WaveX_SPI_TxRxCpltCallback(SPI_HandleTypeDef *h)
{
    // Note: This requires actual SPI handle configuration
    // if (h != &hspi2) return;

    static int phase = 0;
    pkt_t *rx = using_A ? &rxA : &rxB;
    pkt_t *tx = using_A ? &txA : &txB;

    if (phase == 0) {
        // Header just completed; start tail
        uint16_t n = (rx->h.len <= sizeof(rx->payload)) ? rx->h.len : 0;
        // HAL_SPI_TransmitReceive_DMA(&hspi2, tx->payload, rx->payload, n + 2 /*+crc*/);
        phase = 1;
        return;
    }

    // Tail done: validate RX, queue if good
    phase = 0;
    uint16_t rx_crc; 
    memcpy(&rx_crc, rx->payload + rx->h.len, 2);
    rx->crc = rx_crc;
    
    if (rx->h.len <= sizeof(rx->payload) && pkt_crc(rx) == rx_crc) {
        // Hand to app
        pkt_t *slot = (pkt_t*)ring_pop(&free_rx);
        if (slot) { 
            *slot = *rx; 
            ring_push(&rx_q, slot); 
            s_stats.packets_received++;
        }
    } else {
        s_stats.crc_errors++;
    }

    // Flip buffers and re-prime for the next CS burst
    using_A ^= 1;
    prime_next_buffers();
}

extern "C" void WaveX_SPI_ErrorCallback(SPI_HandleTypeDef *h)
{
    // Note: This requires actual SPI handle configuration
    // if (h != &hspi2) return;
    // Re-prime on error
    using_A ^= 1;
    prime_next_buffers();
}

// ---- Public API for app -----------------------------------------------------
namespace WaveX {
namespace Comm {

void Spi_Init(daisy::DaisySeed& hw)
{
    hw.PrintLine("Initializing SPI link...");
    
    init_pools();

    // Configure SPI2 as SLAVE, CPOL=0, CPHA=1EDGE, 8-bit, NSS hardware input (recommended).
    // In CubeMX: Enable DMA Tx/Rx (circular not required), link callbacks.
    // Make IRQ pin output (idle high).
    irq_release();

    // Prepare first DMA set; the ESP master will assert CS and clock us anytime.
    using_A = 1; 
    prime_next_buffers();
    
    hw.PrintLine("SPI link initialized successfully");
}

int Spi_Send(uint16_t type, const void* payload, uint16_t len)
{
    pkt_t *p = (pkt_t*)ring_pop(&free_tx); 
    if (!p) return 0;
    
    pkt_fill(p, type, payload, len);
    int ok = ring_push(&tx_q, p);
    if (ok) {
        irq_assert();    // tell ESP we have data
        s_stats.packets_sent++;
        s_stats.irq_asserts++;
    }
    return ok;
}

int Spi_Recv(pkt_t **out)
{
    *out = (pkt_t*)ring_pop(&rx_q);
    if (*out) return 1;
    return 0;
}

void Spi_Recycle(pkt_t *p, int is_rx) {
    if (is_rx) ring_push(&free_rx, p); 
    else ring_push(&free_tx, p);
}

bool Spi_HasPendingData(void) {
    return !ring_empty(&rx_q);
}

void Spi_GetStats(spi_link_stats_t* stats) {
    if (stats) {
        memcpy(stats, &s_stats, sizeof(s_stats));
    }
}

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED
