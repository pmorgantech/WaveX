#include "inter_spi.h"

#if WAVEX_SPI_LINK_ENABLED

#include "daisy_seed.h"
#include <string.h>
#include <stdint.h>
#include "per/gpio.h"
#include "per/spi.h"
#include "sys/system.h" // For System::DelayUs
#include "sys/dma.h"    // For DMA cache helpers
#include "config/link_config.h" // For HD protocol command macros
#include "config/pin_config.h" // For Daisy SPI pin definitions

using namespace daisy;

// Control packet structure (matching ESP32 slave format)
#define CTRL_PKT_SIZE 64 // Must be 64 for ESP32-P4 DMA alignment

typedef struct __attribute__((packed)) {
    uint8_t type, flags, seq, len;
    uint8_t payload[26];
    uint16_t crc; // CRC16-CCITT over first 30 bytes
    uint8_t padding[32];
} ctrl_pkt_t;

// CRC16-CCITT implementation
static uint16_t crc16_ccitt(const uint8_t* d, size_t n)
{
    uint16_t c=0xFFFF;
    for(size_t i=0;i<n;i++){ 
        c ^= (uint16_t)d[i]<<8; 
        for(int b=0;b<8;b++) 
            c=(c&0x8000)?(c<<1)^0x1021:(c<<1); 
    }
    return c;
}

// ============================================================================
// Module-level static variables
// ============================================================================

static daisy::DaisySeed* s_hw = NULL;
static daisy::SpiHandle* g_spi_handle = NULL;
static daisy::GPIO cs_pin;
static WaveX::Comm::spi_link_stats_t s_stats = {};

#if WAVEX_SPI_DMA_ENABLED
static volatile bool s_tx_inflight = false;
#ifdef DMA_BUFFER_MEM_SECTION
static uint8_t DMA_BUFFER_MEM_SECTION s_tx_dma_buf[CTRL_PKT_SIZE];
#else
static uint8_t s_tx_dma_buf[CTRL_PKT_SIZE];
#endif

static void spi_dma_start_cb(void* /*context*/)
{
    // Assert CS at the moment DMA actually starts
    cs_pin.Write(false);
}

static void spi_dma_end_cb(void* /*context*/, daisy::SpiHandle::Result result)
{
    // Deassert CS and update state
    cs_pin.Write(true);
    if(result == daisy::SpiHandle::Result::OK)
    {
        s_stats.packets_sent++;
    }
    s_tx_inflight = false;
}
#endif

// ============================================================================
// Low-Level SPI Transfer Functions (static to this file)
// ============================================================================

/**
 * @brief Send a single 64-byte frame to ESP32. Uses DMA when enabled.
 */
static daisy::SpiHandle::Result Spi_SendRaw64(const uint8_t* tx_buf)
{
    if(!g_spi_handle)
        return daisy::SpiHandle::Result::ERR;

#if WAVEX_SPI_DMA_ENABLED
    if(s_tx_inflight)
        return daisy::SpiHandle::Result::ERR;

    // Copy payload into persistent DMA buffer
    memcpy(s_tx_dma_buf, tx_buf, CTRL_PKT_SIZE);
    // Ensure cache coherency for DMA
    dsy_dma_clear_cache_for_buffer(s_tx_dma_buf, CTRL_PKT_SIZE);

    s_tx_inflight = true;
    // Start DMA transfer; CS low/high handled in callbacks
    return g_spi_handle->DmaTransmit(
        s_tx_dma_buf,
        CTRL_PKT_SIZE,
        spi_dma_start_cb,
        spi_dma_end_cb,
        NULL);
#else
    // Fallback: blocking single-frame transmit
    cs_pin.Write(false);
    daisy::SpiHandle::Result res
        = g_spi_handle->BlockingTransmit((uint8_t*)tx_buf, CTRL_PKT_SIZE, 100);
    cs_pin.Write(true);
    return res;
#endif
}

// Receive path currently unused with regular SPI slave link. Stubbed for now.
static daisy::SpiHandle::Result Spi_RecvRaw64(uint8_t* /*rx_buf*/)
{
    return daisy::SpiHandle::Result::ERR;
}


// ============================================================================
// Public API Implementation
// ============================================================================

namespace WaveX {
namespace Comm {

void Spi_Init(daisy::DaisySeed &hw, daisy::SpiHandle* hspi)
{
    s_hw = &hw;
    hw.PrintLine("SPI Init: Daisy Master for ESP32 Slave");
    
    if (!hspi) {
        hw.PrintLine("ERROR: SPI handle is null.");
        return;
    }

    // 1) Claim CS as a plain GPIO *before* SPI init
    daisy::Pin cs_p = hw.GetPin(WAVEX_DAISY_SPI_CS);
    cs_pin.Init(cs_p, daisy::GPIO::Mode::OUTPUT, daisy::GPIO::Pull::PULLUP, daisy::GPIO::Speed::VERY_HIGH);
    cs_pin.Write(true); // Start with CS high (inactive)

    // 2) Configure SPI1 Master with NSS::SOFT and NO nss pin assigned
    daisy::SpiHandle::Config spi_config;
    spi_config.periph = daisy::SpiHandle::Config::Peripheral::SPI_1;
    spi_config.mode = daisy::SpiHandle::Config::Mode::MASTER;
    spi_config.direction = daisy::SpiHandle::Config::Direction::TWO_LINES;
    spi_config.datasize = 8;
    spi_config.clock_polarity = daisy::SpiHandle::Config::ClockPolarity::LOW;
    spi_config.clock_phase = daisy::SpiHandle::Config::ClockPhase::ONE_EDGE;
    spi_config.baud_prescaler = daisy::SpiHandle::Config::BaudPrescaler::PS_8;
    spi_config.nss = daisy::SpiHandle::Config::NSS::SOFT;
    
    spi_config.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    spi_config.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    spi_config.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    spi_config.pin_config.nss  = Pin(); // IMPORTANT: leave unassigned
    
    SpiHandle::Result result = hspi->Init(spi_config);
    
    if (result == SpiHandle::Result::OK) {
        hw.PrintLine("SUCCESS: SPI master configured correctly!");
        g_spi_handle = hspi;
        memset(&s_stats, 0, sizeof(s_stats));
    } else {
        hw.PrintLine("ERROR: SPI init failed with result: %d", (int)result);
        g_spi_handle = NULL;
    }
}

int Spi_Send(uint16_t type, const void* payload, uint16_t len)
{
    if (!g_spi_handle) return 0;
    
    ctrl_pkt_t tx_pkt = {0};
    tx_pkt.type = (uint8_t)(type & 0xFF);
    tx_pkt.seq = s_stats.packets_sent & 0xFF;
    tx_pkt.len = (uint8_t)(len > 26 ? 26 : len);
    
    if (payload && len > 0) {
        memcpy(tx_pkt.payload, payload, tx_pkt.len);
    }
    tx_pkt.crc = crc16_ccitt((uint8_t*)&tx_pkt, 30);

    daisy::SpiHandle::Result write_res = Spi_SendRaw64((uint8_t*)&tx_pkt);

    // In DMA mode, completion is accounted in end-callback; here we only
    // indicate whether submission succeeded.
    return write_res == daisy::SpiHandle::Result::OK ? 1 : 0;
}

int Spi_Poll_For_Response(void)
{
    // Regular SPI slave currently does not push responses; not used.
    return 0;
}


// Stubs for the rest of the API
int Spi_Recv(pkt_t **out) { *out = NULL; return 0; }
void Spi_ValidateBuffers() {}
void Spi_Recycle(pkt_t *p, int is_rx) {}
void Spi_ForceReset() {}
bool Spi_HasPendingData(void) { return false; }

void Spi_GetStats(spi_link_stats_t* stats) {
    if (stats) memcpy(stats, &s_stats, sizeof(s_stats));
}

void Spi_DebugState() {
    if (s_hw) {
        s_hw->PrintLine("SPI Master Debug State:");
        s_hw->PrintLine("  Stats: TX=%lu, RX=%lu, CRC_ERR=%lu", 
                        (unsigned long)s_stats.packets_sent,
                        (unsigned long)s_stats.packets_received, 
                        (unsigned long)s_stats.crc_errors);
    }
}

} // namespace Comm
} // namespace WaveX

#endif // WAVEX_SPI_LINK_ENABLED
