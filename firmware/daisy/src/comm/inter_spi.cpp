#include "inter_spi.h"

#if WAVEX_SPI_LINK_ENABLED

#include "stm32h7xx_hal.h"
#include "daisy_seed.h"
#include <string.h>
#include <stdint.h>
#include <cstddef>  // Add this for NULL
#include "per/gpio.h"
#include "per/spi.h"
#include "../../shared/spi_protocol/spi_protocol.h"
#include "util/scopedirqblocker.h"
#include "sys/dma.h"  // Add this for cache management functions

// Direct STM32 HAL SPI implementation
static SPI_HandleTypeDef hspi1;
static DMA_HandleTypeDef hdma_spi1_tx;
static DMA_HandleTypeDef hdma_spi1_rx;

// Use the libDaisy Gpio object for safe hardware access
static daisy::GPIO irq_pin;

// Add at top after existing static declarations
static daisy::DaisySeed* s_hw;

// Add forward declarations after existing static variables
static inline void irq_assert();
static inline void irq_release();

// ---- Queues and pools -------------------------------------------------------
static ring_t rx_q, tx_q;
static void *rx_backing[SPI_RX_RING_SIZE], *tx_backing[SPI_TX_RING_SIZE];
static pkt_t rx_pool[SPI_POOL_SIZE], tx_pool[SPI_POOL_SIZE];
static ring_t free_rx, free_tx;
static void*  backing_frx[SPI_POOL_SIZE], *backing_ftx[SPI_POOL_SIZE];

// Statistics
static WaveX::Comm::spi_link_stats_t s_stats = {};

// Global variables for DMA packet handling
static pkt_t g_current_tx_packet;
static volatile bool g_packet_ready = false;
static volatile bool g_dma_setup = false;  // Add this variable
static daisy::SpiHandle* g_spi_handle = NULL;

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

// IRQ control functions - using the Gpio object
static inline void irq_assert()
{
    irq_pin.Init(s_hw->GetPin(13), daisy::GPIO::Mode::OUTPUT);  // Use D13 as per pin_config.h
    irq_pin.Write(false); // Drive IRQ line low
}
static inline void irq_release()
{
    irq_pin.Init(s_hw->GetPin(13), daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP); // Set to high-impedance with pull-up
}

// ---- Public API for app -----------------------------------------------------
namespace WaveX {
namespace Comm {

// 1. DMA buffers in non-cacheable memory
static pkt_t g_tx_buffer DMA_BUFFER_MEM_SECTION;
static pkt_t g_rx_buffer DMA_BUFFER_MEM_SECTION;

// Forward declarations for functions used within namespace
static void prime_tx_and_start_dma();
static void dma_complete_callback(void* context, daisy::SpiHandle::Result result);

// 3. Proper TX buffer priming and cache management
static void prime_tx_and_start_dma() {
    if (g_dma_setup || !g_spi_handle) {
        return; // Already setup or no handle
    }
    
    // Initialize with empty packet - but keep existing data if packet is ready
    if (!g_packet_ready) {
        memset(&g_tx_buffer, 0, sizeof(g_tx_buffer));
    }
    memset(&g_rx_buffer, 0, sizeof(g_rx_buffer));
    
    // CRITICAL: Clear cache before DMA as recommended
    // This ensures DMA reads fresh data from cache to SRAM
    dsy_dma_clear_cache_for_buffer((uint8_t*)&g_tx_buffer, sizeof(pkt_t));
    dsy_dma_clear_cache_for_buffer((uint8_t*)&g_rx_buffer, sizeof(pkt_t));
    
    s_hw->PrintLine("DEBUG: Starting DMA with TX buffer at %p, RX buffer at %p", 
                    &g_tx_buffer, &g_rx_buffer);
    
    // Start DMA with primed buffers - this is the KEY: TX must be ready BEFORE CS falls
    daisy::SpiHandle::Result result = g_spi_handle->DmaTransmitAndReceive(
        (uint8_t*)&g_tx_buffer,
        (uint8_t*)&g_rx_buffer,
        sizeof(pkt_t),
        NULL,
        dma_complete_callback,
        NULL
    );
    
    if (result == daisy::SpiHandle::Result::OK) {
                    s_hw->PrintLine("SUCCESS: DMA started with primed TX buffer, size=%u bytes", (unsigned)sizeof(pkt_t));
        g_dma_setup = true;
    } else {
        s_hw->PrintLine("ERROR: DMA start failed: %d", (int)result);
    }
}

// 4. Proper DMA completion callback with cache management  
static void dma_complete_callback(void* context, daisy::SpiHandle::Result result) {
    // CRITICAL: Invalidate RX cache after DMA completion
    // This ensures CPU reads fresh data from SRAM to cache
    dsy_dma_invalidate_cache_for_buffer((uint8_t*)&g_rx_buffer, sizeof(pkt_t));
    
    s_hw->PrintLine("DEBUG: DMA transfer completed, result: %d", (int)result);
    
    // Check and process received data
    pkt_hdr_t zero_hdr = {};
    if (memcmp(&g_rx_buffer.h, &zero_hdr, sizeof(zero_hdr)) != 0) {
        // Valid packet received - validate CRC and enqueue it
        uint16_t calc_crc = pkt_crc(&g_rx_buffer);
        if (calc_crc == g_rx_buffer.crc) {
            pkt_t* rx_pkt = (pkt_t*)ring_pop(&free_rx);
            if (rx_pkt) {
                memcpy(rx_pkt, &g_rx_buffer, sizeof(pkt_t));
                ring_push(&rx_q, rx_pkt);
                s_stats.packets_received++;
                s_hw->PrintLine("DEBUG: Valid packet received: type=0x%04X len=%u", 
                               rx_pkt->h.type, rx_pkt->h.len);
            } else {
                s_stats.rx_q_overflows++;
                s_hw->PrintLine("WARNING: RX packet dropped - pool exhausted");
            }
        } else {
            s_stats.crc_errors++;
            s_hw->PrintLine("WARNING: RX packet CRC error");
        }
    }
    
    // If we had a packet to send, mark it as successfully transmitted
    if (g_packet_ready) {
        g_packet_ready = false;
        irq_release();
        s_stats.packets_sent++;
        s_hw->PrintLine("DEBUG: TX packet sent successfully");
    }
    
    // CRITICAL: Re-prime and restart DMA immediately for continuous operation
    // This ensures we're always ready for the next ESP32 transaction
    g_dma_setup = false;
    prime_tx_and_start_dma();
}

// 2. Correct SPI slave configuration
void Spi_Init(daisy::DaisySeed &hw, daisy::SpiHandle* hspi)
{
    s_hw = &hw;
    hw.PrintLine("SPI Init with proper slave configuration...");

    init_pools();
    
    // Initialize IRQ pin  
    irq_pin.Init(hw.GetPin(13), daisy::GPIO::Mode::OUTPUT);  // Use D13 as per pin_config.h
    irq_release();
    
    if (hspi) {
        daisy::SpiHandle::Config spi_config;
        spi_config.periph = daisy::SpiHandle::Config::Peripheral::SPI_1;
        spi_config.mode = daisy::SpiHandle::Config::Mode::SLAVE;
        spi_config.direction = daisy::SpiHandle::Config::Direction::TWO_LINES;
        spi_config.datasize = 8;
        
        // CRITICAL: Match ESP32 master Mode 0 (CPOL=0, CPHA=0)
        // Mode 0: Data sampled on rising edge, shifted on falling edge
        spi_config.clock_polarity = daisy::SpiHandle::Config::ClockPolarity::LOW;
        spi_config.clock_phase = daisy::SpiHandle::Config::ClockPhase::ONE_EDGE;
        
        // CRITICAL: Use HARD_INPUT NSS as recommended
        spi_config.nss = daisy::SpiHandle::Config::NSS::HARD_INPUT;
        
        // Pin configuration - using Daisy Seed pin numbers
        spi_config.pin_config.sclk = hw.GetPin(8);   // D8 = SCK
        spi_config.pin_config.mosi = hw.GetPin(10);  // D10 = MOSI  
        spi_config.pin_config.miso = hw.GetPin(9);   // D9 = MISO
        spi_config.pin_config.nss = hw.GetPin(7);    // D7 = CS
        
        hw.PrintLine("DEBUG: Initializing SPI with config: Mode=SLAVE, CPOL=LOW, CPHA=ONE_EDGE, NSS=HARD_INPUT");
        hw.PrintLine("DEBUG: Pin mapping: SCK=D8, MOSI=D10, MISO=D9, CS=D7, IRQ=D13");
        
        daisy::SpiHandle::Result result = hspi->Init(spi_config);
        if (result == daisy::SpiHandle::Result::OK) {
            hw.PrintLine("SUCCESS: SPI slave configured correctly in Mode 0!");
            g_spi_handle = hspi;
            
            // Clear statistics
            memset(&s_stats, 0, sizeof(s_stats));
            s_stats.last_activity_ms = hw.system.GetNow();
            
            // 3. Prime TX buffer and start continuous DMA
            // This is CRITICAL - TX must be ready before ESP32 starts clocking
            prime_tx_and_start_dma();
            
            hw.PrintLine("INFO: SPI slave ready for continuous DMA operation");
        } else {
            hw.PrintLine("ERROR: SPI init failed: %d", (int)result);
        }
    }
}

// 5. Corrected send function with proper cache management
int Spi_Send(uint16_t type, const void* payload, uint16_t len)
{
    s_hw->PrintLine("SPI_Send: type=0x%04X, len=%u", type, len);
    
    if (!g_spi_handle) {
        s_hw->PrintLine("ERROR: SPI handle not initialized!");
        return 0;
    }
    
    // Drop packet if previous one is still pending
    if (g_packet_ready) {
        s_hw->PrintLine("WARNING: Dropping packet, previous still pending");
        s_stats.irq_asserts++; // Count as dropped
        return 0;
    }
    
    // Fill TX buffer with new packet
    pkt_fill(&g_tx_buffer, type, payload, len);
    // Zero-pad remainder of payload to avoid stale bytes beyond len
    if (len < sizeof(g_tx_buffer.payload)) {
        memset(g_tx_buffer.payload + len, 0, sizeof(g_tx_buffer.payload) - len);
    }
    
    // CRITICAL: Clear cache before DMA reads the data
    // This ensures DMA sees the fresh packet data we just wrote
    dsy_dma_clear_cache_for_buffer((uint8_t*)&g_tx_buffer, sizeof(pkt_t));
    
    // Mark packet as ready and PRIME DMA BEFORE signaling master
    g_packet_ready = true;
    // Optional: log first 8 bytes of TX buffer for validation on master
    {
        const uint8_t *txb = (const uint8_t*)&g_tx_buffer;
        s_hw->PrintLine("DEBUG: TX header bytes [0-7]: %02X %02X %02X %02X %02X %02X %02X %02X",
                        txb[0], txb[1], txb[2], txb[3], txb[4], txb[5], txb[6], txb[7]);
    }

    // Ensure DMA uses the freshly prepared TX buffer
    g_dma_setup = false;
    prime_tx_and_start_dma();

    // Small delay to allow DMA to prime the SPI TX FIFO before master clocks
    s_hw->DelayMs(1);

    // Now signal ESP32 that data is ready
    irq_assert();
    s_stats.irq_asserts++;
    
    s_hw->PrintLine("DEBUG: Packet queued - type=0x%04X, len=%u, IRQ asserted", type, len);
    
    // Update statistics
    s_stats.last_activity_ms = s_hw->system.GetNow();
    
    return 1;
}

int Spi_Recv(pkt_t **out)
{
    *out = (pkt_t*)ring_pop(&rx_q);
    if (*out) return 1;
    return 0;
}

// Add a function to validate the DMA buffer setup
void Spi_ValidateBuffers() {
    if (s_hw) {
        s_hw->PrintLine("Buffer validation:");
        s_hw->PrintLine("  TX buffer at: %p (section: DMA_BUFFER_MEM_SECTION)", &g_tx_buffer);
        s_hw->PrintLine("  RX buffer at: %p (section: DMA_BUFFER_MEM_SECTION)", &g_rx_buffer);
        s_hw->PrintLine("  Buffer size: %u bytes each", (unsigned)sizeof(pkt_t));
        
        // Check if buffers are in the expected memory region
        uint32_t tx_addr, rx_addr;
        tx_addr = (uint32_t)&g_tx_buffer;
        rx_addr = (uint32_t)&g_rx_buffer;
        
        // SRAM1 typically starts around 0x30000000 on STM32H7
        if (tx_addr >= 0x30000000 && tx_addr < 0x30020000) {
            s_hw->PrintLine("  ✓ TX buffer in SRAM1 (non-cacheable)");
        } else {
            s_hw->PrintLine("  ⚠ TX buffer NOT in expected SRAM1 region");
        }
        
        if (rx_addr >= 0x30000000 && rx_addr < 0x30020000) {
            s_hw->PrintLine("  ✓ RX buffer in SRAM1 (non-cacheable)");
        } else {
            s_hw->PrintLine("  ⚠ RX buffer NOT in expected SRAM1 region");
        }
    }
}

void Spi_Recycle(pkt_t *p, int is_rx) {
    if (is_rx) ring_push(&free_rx, p); 
    else ring_push(&free_tx, p);
}

// Add a function to manually reset stuck transfers
void Spi_ForceReset() {
    if (s_hw) {
        s_hw->PrintLine("FORCE RESET: Clearing stuck packet state...");
        
        // Release any stuck IRQ
        irq_release();
        
        // Clear packet ready flag
        if (g_packet_ready) {
            s_hw->PrintLine("  Cleared stuck packet_ready flag");
            g_packet_ready = false;
        }
        
        // If DMA is stuck, restart it
        if (g_dma_setup && g_spi_handle) {
            s_hw->PrintLine("  Restarting DMA...");
            g_dma_setup = false;
            prime_tx_and_start_dma();
        }
        
        s_hw->PrintLine("  Reset complete - ready for new transfers");
    }
}

bool Spi_HasPendingData(void) {
    return !ring_empty(&rx_q);
}

void Spi_GetStats(spi_link_stats_t* stats) {
    if (stats) {
        memcpy(stats, &s_stats, sizeof(s_stats));
    }
}

// Debug function to check current state
void Spi_DebugState() {
    if (s_hw) {
        s_hw->PrintLine("SPI Debug State:");
        s_hw->PrintLine("  PacketReady=%d, DmaSetup=%d, IRQ=%s", 
                        g_packet_ready ? 1 : 0, 
                        g_dma_setup ? 1 : 0,
                        irq_pin.Read() ? "IDLE" : "ASSERTED");  // IRQ is active low
        s_hw->PrintLine("  Stats: TX=%lu, RX=%lu, CRC_ERR=%lu, Overflows=%lu, IRQ_Asserts=%lu", 
                        (unsigned long)s_stats.packets_sent,
                        (unsigned long)s_stats.packets_received, 
                        (unsigned long)s_stats.crc_errors,
                        (unsigned long)s_stats.rx_q_overflows,
                        (unsigned long)s_stats.irq_asserts);
        s_hw->PrintLine("  Queue status: RX_pending=%d, Last_activity=%lu ms ago",
                        !ring_empty(&rx_q) ? 1 : 0,
                        (unsigned long)(s_hw->system.GetNow() - s_stats.last_activity_ms));
        
        // Show buffer addresses to confirm non-cacheable placement
        s_hw->PrintLine("  Buffers: TX=0x%08X RX=0x%08X", 
                        (unsigned int)&g_tx_buffer, (unsigned int)&g_rx_buffer);
    }
}



} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED
