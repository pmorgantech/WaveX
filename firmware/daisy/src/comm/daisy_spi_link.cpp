#include "daisy_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

// Force platform define for linter
#ifndef DAISY_PLATFORM
#define DAISY_PLATFORM 1
#endif

#include "daisy_seed.h"
#include <string.h>
#include <stdint.h>
#include <cstdint>
#include <cstdlib>
#include "per/gpio.h"
#include "per/spi.h"
#include "sys/system.h" // For System::DelayUs
#include "sys/dma.h"    // For DMA cache helpers
#include "daisy_core.h" // For DMA_BUFFER_MEM_SECTION
#include "stm32h7xx_hal.h" // For SCB cache maintenance functions and STM32 HAL GPIO and interrupt functions
#include "stm32h7xx_hal_cortex.h" // For SCB cache maintenance functions
#include "stm32h7xx_hal_crc.h" // For hardware CRC support
#include "config/link_config.h" // For HD protocol command macros
#include "config/pin_config.h" // For Daisy SPI pin definitions
#include "spi_protocol/protocol.h" // For protocol functions
#include "config/logging_config.h" // For logging macros
#include "../storage/fs_browse.h" // For file browsing
#include "../audio/audio_engine.h" // For sample audition
#include "daisy_spi_message_handlers.h" // For message handlers
#include "daisy_spi_filesystem.h" // For filesystem function declarations

using namespace daisy;
using namespace WaveX::Protocol;

// Use shared CRC function
#define crc16_ccitt ProtocolHandler::CalculateSpiCrc

// Use new unified packet system functions
#define get_packet_size_from_code ProtocolHandler::GetPacketSizeFromCode
#define get_optimal_size_code ProtocolHandler::GetOptimalSizeCode
// Use hardware CRC when available, fallback to software
// #define calculate_wave_crc calculate_hardware_crc16
#define calculate_wave_crc ProtocolHandler::CalculateWaveXCrc
#define validate_wave_packet ProtocolHandler::ValidateWaveXPacket
#define create_wave_packet ProtocolHandler::CreateWaveXPacket
#define parse_wave_packet ProtocolHandler::ParseWaveXPacket

// Legacy function compatibility (for functions that were removed)
#define validate_packet validate_wave_packet

// How long to wait for ESP32 to be ready for DMA
#define ESP32_DMA_READY_WAIT_MS 1000


// Compatibility function for get_packet_size - extracts size code from packet data
static size_t get_packet_size(const uint8_t* packet_data) {
    if (!packet_data) return 0;
    uint8_t size_code = packet_data[0] & PKT_SIZE_MASK;
    return get_packet_size_from_code(size_code);
}

// ============================================================================
// Module-level static variables
// ============================================================================

// Forward declarations for debug function and DMA callbacks
static void spi_dma_start_cb(void* context);
static void spi_dma_end_cb(void* context, daisy::SpiHandle::Result result);

daisy::DaisySeed* s_hw = NULL;
static daisy::SpiHandle* g_spi_handle = NULL;
static daisy::GPIO cs_pin;
static daisy::GPIO attn_pin;
static WaveX::Comm::spi_link_stats_t s_stats = {};

// ============================================================================
// Message Queue and Packet Management
// ============================================================================

// Message queue for received messages (unified for both packet types)
#define MAX_QUEUED_MESSAGES 8

// Sequence number tracking for duplicate detection
static uint16_t s_last_received_seq = 0;
static uint16_t s_expected_seq = 1; // Start from 1, 0 is reserved
static uint32_t s_duplicate_count = 0;
static uint32_t s_out_of_order_count = 0;

// ============================================================================
// Utility Functions (used by message handlers)
// ============================================================================

namespace WaveX {
namespace Comm {

// Check for duplicate packets using sequence numbers (packet-level processing)
static bool is_duplicate_packet(uint16_t seq_num)
{
    if (seq_num == 0) {
        // Sequence number 0 is reserved/invalid
        return true;
    }
    
    if (seq_num == s_last_received_seq) {
        // Exact duplicate
        s_duplicate_count++;
        if (s_hw) s_hw->PrintLine("DAISY: Duplicate packet detected: seq=%u (duplicate count: %u)", seq_num, s_duplicate_count);
        return true;
    }
    
    // Check for out-of-order packets (allow some tolerance for network reordering)
    uint16_t expected_min = (s_expected_seq > 10) ? (s_expected_seq - 10) : 1;
    if (seq_num < expected_min) {
        s_out_of_order_count++;
        if (s_hw) s_hw->PrintLine("DAISY: Out-of-order packet: seq=%u, expected>=%u (out-of-order count: %u)", 
                                  seq_num, expected_min, s_out_of_order_count);
        return true;
    }
    
    // Update tracking
    s_last_received_seq = seq_num;
    s_expected_seq = seq_num + 1;
    
    return false;
}

} // namespace Comm
} // namespace WaveX

// Software CRC16 calculation matching ESP32 and shared protocol (CRC-16-CCITT)
static uint16_t calculate_hardware_crc16(const uint8_t* data, size_t length)
{
    if (!data || length == 0) return 0;
    
    // Use CRC-16-CCITT algorithm matching ESP32 and shared protocol exactly
    // Polynomial: 0x1021 (CRC-16-CCITT)
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)(data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc & 0xFFFF;
}

// CRC16 initialization (software implementation - no hardware needed)
static bool init_hardware_crc()
{
    if (s_hw) s_hw->PrintLine("DAISY: CRC16 initialized (software implementation matching ESP32)");
    return true;
}

// Forward declarations will be inside namespace

#if WAVEX_SPI_DMA_ENABLED
static volatile bool s_tx_inflight = false;
static volatile bool s_duplex_inflight = false;
static volatile uint32_t s_packets_ready_for_processing = 0; // Counter instead of bool
static uint32_t s_dma_start_time = 0;
static const uint32_t DMA_TIMEOUT_MS = 100; // 100ms timeout

// DMA buffers - these are for active DMA transfers
static uint8_t s_tx_dma_buf[MAX_PKT_SIZE] DMA_BUFFER_MEM_SECTION;
static uint8_t s_rx_dma_buf[MAX_PKT_SIZE] DMA_BUFFER_MEM_SECTION;

// Multiple TX buffers for send throughput - in regular RAM
#define TX_BUFFER_COUNT 4
static uint8_t s_tx_buffers[TX_BUFFER_COUNT][MAX_PKT_SIZE] __attribute__((aligned(32)));
static volatile uint32_t s_tx_buffer_states[TX_BUFFER_COUNT]; // 0=free, 1=ready, 2=sending
static volatile uint32_t s_current_tx_buffer = 0;
static volatile uint32_t s_tx_packets_queued = 0;

// Multiple RX buffers for receive throughput - in regular RAM  
#define RX_BUFFER_COUNT 4
static uint8_t s_rx_buffers[RX_BUFFER_COUNT][MAX_PKT_SIZE] __attribute__((aligned(32)));
static volatile uint32_t s_current_rx_buffer = 0; // Currently active RX buffer for DMA
static volatile uint32_t s_ready_rx_buffers[RX_BUFFER_COUNT]; // Buffer ready flags (0=free, 1=ready)

// Static packet buffer pool for large packets (replaces malloc/free)
static uint8_t s_large_packet_pool[4][MAX_PKT_SIZE] __attribute__((aligned(32)));
static bool s_pool_used[4] = {false, false, false, false};

// Polling backpressure - minimum interval between polls when idle
static uint32_t s_last_poll_time = 0;
static const uint32_t MIN_POLL_INTERVAL_MS = 5; // Increased from 1ms to 5ms for better CPU efficiency

static void spi_dma_start_cb(void* /*context*/)
{
    // Assert CS at the moment DMA actually starts
    cs_pin.Write(false);
    // Set flag to indicate DMA is running
    s_tx_inflight = true;
}

static void spi_dma_end_cb(void* /*context*/, daisy::SpiHandle::Result result)
{
    // Deassert CS and update state
    cs_pin.Write(true);
    
    if(result == daisy::SpiHandle::Result::OK) {
        s_stats.packets_sent++;
        
        // Mark current TX buffer as free (minimal IRQ work)
        if (s_current_tx_buffer < TX_BUFFER_COUNT) {
            s_tx_buffer_states[s_current_tx_buffer] = 0; // Mark as free
        }
    }
    s_tx_inflight = false;
}

static void spi_duplex_start_cb(void* /*context*/)
{
    // Assert CS at the moment DMA actually starts
    cs_pin.Write(false);
    s_duplex_inflight = true;
}

static void spi_duplex_end_cb(void* /*context*/, daisy::SpiHandle::Result result)
{
    // Deassert CS and update state
    cs_pin.Write(true);
    
    if(result == daisy::SpiHandle::Result::OK) {
        s_stats.packets_received++;
        
        // ONLY set flags in IRQ context - NO memory operations!
        // The DMA buffer data will be copied in main loop processing
        s_packets_ready_for_processing++;
    }
    s_duplex_inflight = false;
}

// Get a free packet buffer from the pool
static uint8_t* get_packet_buffer(size_t size)
{
    if (size > MAX_PKT_SIZE) return nullptr;
    
    for (int i = 0; i < 4; i++) {
        if (!s_pool_used[i]) {
            s_pool_used[i] = true;
            return s_large_packet_pool[i];
        }
    }
    return nullptr; // All buffers in use
}

// Return a packet buffer to the pool
static void release_packet_buffer(uint8_t* buffer)
{
    for (int i = 0; i < 4; i++) {
        if (s_large_packet_pool[i] == buffer) {
            s_pool_used[i] = false;
            return;
        }
    }
}
#endif

// ============================================================================
// Low-Level SPI Transfer Functions (static to this file)
// ============================================================================

// ============================================================================
// SPI Transaction with Response Reception
// ============================================================================

/**
 * @brief Send a packet to ESP32 (one-way). Uses DMA when enabled.
 * Supports both command (32-byte) and data (256-byte) packets.
 */
static daisy::SpiHandle::Result Spi_SendPacket(const uint8_t* tx_buf, size_t packet_size)
{
    if(!g_spi_handle) {
        if (s_hw) s_hw->PrintLine("Spi_SendPacket ERROR: g_spi_handle is NULL!");
        return daisy::SpiHandle::Result::ERR;
    }

    // Validate packet size - allow flexible packet sizes
    if (packet_size != 32 && packet_size != 64 &&
        packet_size != 128 && packet_size != 256 &&
        packet_size != 512 && packet_size != 1024 &&
        packet_size != 2048) {
        if (s_hw) s_hw->PrintLine("Spi_SendPacket ERROR: Invalid packet size: %d", (int)packet_size);
        return daisy::SpiHandle::Result::ERR;
    }

#if WAVEX_SPI_DMA_ENABLED
    if(s_tx_inflight) {
        if (s_hw) s_hw->PrintLine("Spi_SendPacket: DMA transaction already in flight, returning ERR");
        if (s_hw) s_hw->PrintLine("DAISY: Previous transaction started at time=%u, current time=%u", s_dma_start_time, System::GetTick());
        return daisy::SpiHandle::Result::ERR;
    }

    // Force clear any potential stuck state from previous failed transactions
    s_tx_inflight = false;
    cs_pin.Write(true); // Ensure CS is high before starting

    // Find a free TX buffer and prepare it
    uint32_t tx_buffer_idx = 0;
    bool found_free = false;
    for (uint32_t i = 0; i < TX_BUFFER_COUNT; i++) {
        if (s_tx_buffer_states[i] == 0) { // Free
            tx_buffer_idx = i;
            found_free = true;
            break;
        }
    }
    
    if (!found_free) {
        if (s_hw) s_hw->PrintLine("DAISY: All TX buffers in use - dropping packet");
        return daisy::SpiHandle::Result::ERR;
    }
    
    // Check if packet fits in DMA buffer
    if (packet_size > MAX_PKT_SIZE) {
        if (s_hw) s_hw->PrintLine("DAISY: Packet too large for DMA buffer (%d > %d)", (int)packet_size, MAX_PKT_SIZE);
        return daisy::SpiHandle::Result::ERR;
    }
    
    // Prepare the TX buffer
    memset(s_tx_buffers[tx_buffer_idx], 0, MAX_PKT_SIZE);
    memcpy(s_tx_buffers[tx_buffer_idx], tx_buf, packet_size);
    s_tx_buffer_states[tx_buffer_idx] = 2; // Mark as sending
    s_current_tx_buffer = tx_buffer_idx;
    
    // Copy to DMA buffer for transmission (only the packet size needed)
    memset(s_tx_dma_buf, 0, MAX_PKT_SIZE);
    memcpy(s_tx_dma_buf, s_tx_buffers[tx_buffer_idx], packet_size);
    
    // Cache operations removed - buffers are in non-cacheable DMA memory

    if (s_hw) s_hw->PrintLine("DAISY: Starting DMA duplex transaction, packet_size=%d", (int)packet_size);
    // if (s_hw) s_hw->PrintLine("DAISY: TX buffer contents: %02X %02X %02X %02X %02X %02X %02X %02X", 
    //                            s_tx_dma_buf[0], s_tx_dma_buf[1], s_tx_dma_buf[2], s_tx_dma_buf[3],
    //                            s_tx_dma_buf[4], s_tx_dma_buf[5], s_tx_dma_buf[6], s_tx_dma_buf[7]);
                               
    // Start DMA duplex transfer
    uint32_t call_start_time = System::GetTick();
    s_dma_start_time = call_start_time; // Set timing before DMA starts
    
    // cs_pin.Write(false);
    daisy::SpiHandle::Result dma_result = g_spi_handle->DmaTransmitAndReceive(
        s_tx_dma_buf,
        s_rx_dma_buf, // Use RX buffer as dummy for send operations
        MAX_PKT_SIZE,
        spi_dma_start_cb,
        spi_dma_end_cb,
        NULL);
    // cs_pin.Write(true); // REMOVED: Let callbacks handle CS timing

    uint32_t call_end_time = System::GetTick();
    uint32_t call_duration = call_end_time - call_start_time;
    
    // if (s_hw) s_hw->PrintLine("DAISY: DmaTransmitAndReceive call completed at time=%u, duration=%u ms", call_end_time, call_duration);
    // if (s_hw) s_hw->PrintLine("DAISY: DmaTransmitAndReceive returned: %d", (int)dma_result);
    // if (s_hw) s_hw->PrintLine("DAISY: Post-DMA state: inflight=%s, start_time=%u", s_tx_inflight ? "true" : "false", s_dma_start_time);
    
    if (dma_result != daisy::SpiHandle::Result::OK) {
        if (s_hw) s_hw->PrintLine("DAISY: DMA transaction failed to start - result=%d, clearing inflight flag", (int)dma_result);
        s_tx_inflight = false;
        cs_pin.Write(true); // Ensure CS is high
        
        // Provide detailed error information
        const char* error_msg = "Unknown error";
        switch (dma_result) {
            case daisy::SpiHandle::Result::OK: error_msg = "Should not be here - OK case"; break;
            case daisy::SpiHandle::Result::ERR: error_msg = "General SPI error"; break;
            default: error_msg = "Unknown result code"; break;
        }
        if (s_hw) s_hw->PrintLine("DAISY: DMA error details: %s", error_msg);
    }
    
    // Add small delay to prevent overwhelming ESP32
    // FIXME: This seems to have an effect on whether or not get receive browse_resp packets
    System::DelayUs(ESP32_DMA_READY_WAIT_MS); // Reduced from 1000us to 400us for better performance
    
    return dma_result;
#else
    // Fallback: blocking duplex transmit with software CS
    // ESP32 slave expects duplex transactions, so we need to transmit AND receive
    if (s_hw) s_hw->PrintLine("DAISY: Starting blocking duplex transmit, packet_size=%d", (int)packet_size);

    // Prepare RX buffer for any response from ESP32
    uint8_t rx_buf[MAX_PKT_SIZE];
    memset(rx_buf, 0, sizeof(rx_buf));

    cs_pin.Write(false);
    System::DelayUs(10); // Small delay for setup

    uint32_t blocking_start = System::GetTick();
    daisy::SpiHandle::Result res = g_spi_handle->BlockingTransmitAndReceive((uint8_t*)tx_buf, rx_buf, packet_size, 10); // Reduced from 1000ms to 10ms
    uint32_t blocking_duration = System::GetTick() - blocking_start;

    cs_pin.Write(true);
    
    if (s_hw) s_hw->PrintLine("DAISY: Blocking duplex transmit %s, duration=%u ms", 
                              res == daisy::SpiHandle::Result::OK ? "successful" : "failed",
                              (unsigned)blocking_duration);

    if (res == daisy::SpiHandle::Result::OK) {
        if (s_hw) s_hw->PrintLine("DAISY: Blocking duplex transmit successful");
        // Process any received data (though we mainly care about transmit succeeding)
        // ESP32 slave may send ACKs or other data
    } else {
        if (s_hw) s_hw->PrintLine("DAISY: Blocking duplex transmit failed: %d", (int)res);
    }

    return res;
#endif
}

// ============================================================================
// Public API Implementation
// ============================================================================

namespace WaveX {
namespace Comm {

// ============================================================================
// Message Queue Variables
// ============================================================================

// Message queue for received messages (unified for both packet types)
static uint8_t message_queue[MAX_QUEUED_MESSAGES][MAX_PKT_SIZE] __attribute__((aligned(4)));
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

// Outgoing message queue (Daisy → ESP32)
static uint8_t outgoing_queue[MAX_QUEUED_MESSAGES][MAX_PKT_SIZE] __attribute__((aligned(4)));
static int outgoing_head = 0;
static int outgoing_tail = 0;
static int outgoing_count = 0;

// ============================================================================
// Static Function Declarations
// ============================================================================

static bool QueueOutgoingMessage(const uint8_t* packet_data, size_t packet_size);
static void PrepareTxBuffer(uint8_t* tx_buf, size_t buf_size);
static bool PerformBidirectionalPoll();
static bool ProcessReceivedPacket(const uint8_t* rx_buf, size_t transfer_size);

// ============================================================================
// Public API Functions
// ============================================================================

void Spi_Init(daisy::DaisySeed &hw, daisy::SpiHandle* hspi)
{
    s_hw = &hw;
    hw.PrintLine("SPI Init: Daisy Master for ESP32 Slave");
    hw.PrintLine("Spi_Init called with hspi=%p", hspi);
    
    // Initialize hardware CRC peripheral
    if (!init_hardware_crc()) {
        hw.PrintLine("WARNING: Hardware CRC initialization failed, falling back to software CRC");
    }
    
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
    spi_config.baud_prescaler = daisy::SpiHandle::Config::BaudPrescaler::PS_16; // Slower clock for debugging
    spi_config.nss = daisy::SpiHandle::Config::NSS::SOFT;
    spi_config.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    spi_config.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    spi_config.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    spi_config.pin_config.nss  = Pin(); // IMPORTANT: leave unassigned for software CS
    
    hw.PrintLine("Spi_Init: About to call hspi->Init");
    SpiHandle::Result result = hspi->Init(spi_config);
    
    // Access DMA streams directly (libDaisy uses DMA2_Stream2/3 for SPI1)
    if (result == SpiHandle::Result::OK) {
        DMA_Stream_TypeDef* tx_stream = DMA2_Stream3; // libDaisy TX stream
        DMA_Stream_TypeDef* rx_stream = DMA2_Stream2; // libDaisy RX stream
        
        hw.PrintLine("TX DMA (DMA2_Stream3): CR=0x%08lx NDTR=%lu PAR=0x%08lx M0AR=0x%08lx",
                    tx_stream->CR,
                    tx_stream->NDTR,
                    tx_stream->PAR,
                    tx_stream->M0AR);
        
        hw.PrintLine("RX DMA (DMA2_Stream2): CR=0x%08lx NDTR=%lu PAR=0x%08lx M0AR=0x%08lx",
                    rx_stream->CR,
                    rx_stream->NDTR,
                    rx_stream->PAR,
                    rx_stream->M0AR);
    }
    
    if (result == SpiHandle::Result::OK) {
        hw.PrintLine("SUCCESS: SPI master configured correctly!");
        // // **** FORCED SPI1 INITIALIZATION ****
        // if (!(SPI1->CR1 & SPI_CR1_SPE))
        // {
        //     SPI1->CR1 |= SPI_CR1_SPE;     // Enable SPI if not already
        // }
        // if (!(SPI1->CR1 & SPI_CR1_CSTART))
        // {
        //     SPI1->CR1 |= SPI_CR1_CSTART;  // Force start
        //     s_hw->PrintLine("DAISY: Forced SPI1->CR1.CSTART=1");
        // }
        // hw.PrintLine("Spi_Init: hspi->Init returned: %d", (int)result);
        // // Force RX FIFO threshold to 1/4 (8-bit mode, RXNE=1 byte)
        // #define SPI_CR2_RXFTCFG_Pos 29U
        // #define SPI_CR2_RXFTCFG_Msk (0x7UL << SPI_CR2_RXFTCFG_Pos)
        // const uint32_t RXFTCFG_Pos = 29;
        // const uint32_t RXFTCFG_Msk = (0x7U << RXFTCFG_Pos);
        // MODIFY_REG(SPI1->CR2, RXFTCFG_Msk, (0x1U << RXFTCFG_Pos));
        // s_hw->PrintLine("DAISY: Forced RXFTCFG=0x%lx", (SPI1->CR2 >> RXFTCFG_Pos) & 0x7);
        // // **** END OF FORCED SPI1 INITIALIZATION ****
        
        // Set g_spi_handle and initialize stats
        g_spi_handle = hspi;
        hw.PrintLine("Spi_Init: g_spi_handle set to %p", g_spi_handle);
        memset(&s_stats, 0, sizeof(s_stats));
        hw.PrintLine("Spi_Init: Stats initialized");
    } else {
        hw.PrintLine("ERROR: SPI init failed with result: %d", (int)result);
        g_spi_handle = NULL;
        hw.PrintLine("Spi_Init: g_spi_handle set to NULL");
    }

    // Configure D0 as input to receive attention signal from ESP32 GPIO31
    daisy::Pin attn_p = hw.GetPin(WAVEX_DAISY_ATTN_IN);  // D0
    attn_pin.Init(attn_p, daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLDOWN, daisy::GPIO::Speed::VERY_HIGH);
    hw.PrintLine("Attention pin D%d configured as input from ESP32 GPIO31", WAVEX_DAISY_ATTN_IN);

    // Configure GPIO interrupt for the attention pin (D0 is PB12, which uses EXTI15_10)
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;  // Interrupt on rising edge (ESP32 signals data ready)
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // Enable interrupt for the attention pin (D0 is PB12, which uses EXTI15_10)
    // Priority must be lower than audio (higher number) to prevent preemption.
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 14, 0); // Priority 14, as per plan
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
    
    hw.PrintLine("ATTN pin interrupt configured - will trigger DMA duplex transaction on ESP32 signal");

}

// ============================================================================
// Static Helper Functions - Queue Management
// ============================================================================

// Prepare TX buffer with outgoing data or ACK packet
static void PrepareTxBuffer(uint8_t* tx_buf, size_t buf_size)
{
    // Check if we have outgoing messages to send
    if (outgoing_count > 0) {
    uint8_t* outgoing_msg = outgoing_queue[outgoing_head];
    size_t packet_size = get_packet_size(outgoing_msg);
    
        if (packet_size <= buf_size) {
            memcpy(tx_buf, outgoing_msg, packet_size);

            // Log outgoing packet
            uint8_t msg_type = outgoing_msg[1];
            WAVEX_LOG_DAISY_OUTBOUND(DAISY_OUTBOUND_SPI, "Prepared outgoing message type=0x%02X, len=%d, remaining=%d",
                                   msg_type, (int)packet_size, outgoing_count);
        } else {
            // Packet too large for buffer, send zeros
            memset(tx_buf, 0, buf_size);
        }
    } else {
        // No outgoing messages, send a proper "no data" response packet
        uint8_t msg_type = WaveX::Protocol::MSG_ACK;
        size_t packet_size = WaveX::Protocol::ProtocolHandler::CreatePacket(
            tx_buf, buf_size, static_cast<WaveX::Protocol::MessageType>(msg_type),
            nullptr, 0, PKT_FLAG_ACK  // ACK flag indicates this is an acknowledgment
        );
        
        if (packet_size > 0) {
            if (s_hw) s_hw->PrintLine("DAISY: Prepared no-data response packet: size=%d", (int)packet_size);
        } else {
            // Fallback to zeros if packet creation failed
            memset(tx_buf, 0, buf_size);
        }
    }
}

// Add message to outgoing queue
static bool QueueOutgoingMessage(const uint8_t* packet_data, size_t packet_size)
{
    if (outgoing_count >= MAX_QUEUED_MESSAGES) {
        return false; // Queue full
    }

    memcpy(outgoing_queue[outgoing_tail], packet_data, packet_size);
    outgoing_tail = (outgoing_tail + 1) % MAX_QUEUED_MESSAGES;
    outgoing_count++;
    
    return true;
}

// ============================================================================
// Static Helper Functions - Packet Processing
// ============================================================================

// Process received packet - validates, parses, and routes to message handlers
static bool ProcessReceivedPacket(const uint8_t* rx_buf, size_t transfer_size)
{
    // if (s_hw) {
    //     s_hw->PrintLine("DAISY: ProcessReceivedPacket called - transfer_size=%d", (int)transfer_size);
    // }

    if (!rx_buf || transfer_size == 0) {
        if (s_hw) s_hw->PrintLine("DAISY: Invalid rx_buf or transfer_size=0");
        return false;
    }

    // Debug: Always show first 8 bytes of received data
    // if (s_hw) {
    //     s_hw->PrintLine("DAISY: Raw RX data: %02X %02X %02X %02X %02X %02X %02X %02X", 
    //                    rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], 
    //                    rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
    // }

    // Check for all-zero packet (ESP32 sends this when it has no data)
    // This prevents ACK ping-pong between ESP32 and Daisy
    bool all_zeros = true;
    for (size_t i = 0; i < transfer_size && i < 32; i++) { // Check first 32 bytes
        if (rx_buf[i] != 0) {
            all_zeros = false;
            break;
        }
    }
    
    if (all_zeros) {
        //if (s_hw) s_hw->PrintLine("DAISY: Received all-zero packet - ignoring");
        return true; // Successfully ignored
    }

    // Debug: Print received packet details
    if (s_hw) {
        s_hw->PrintLine("DAISY: Received packet size: %d", (int)transfer_size);
        s_hw->PrintLine("DAISY: First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                       rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], 
                       rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
        s_hw->PrintLine("DAISY: Last 4 bytes: %02X %02X %02X %02X", 
                       rx_buf[transfer_size-4], rx_buf[transfer_size-3], 
                       rx_buf[transfer_size-2], rx_buf[transfer_size-1]);
    }

    // Try to determine actual packet size from first byte
    uint8_t size_code = rx_buf[0] & 0x0F; // Lower 4 bits contain size code
    size_t expected_size = get_packet_size_from_code(size_code);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Size code=0x%02X, expected_size=%d", size_code, (int)expected_size);
        s_hw->PrintLine("DAISY: First byte analysis: 0x%02X (flags=0x%02X, size_code=0x%02X)", 
                       rx_buf[0], (rx_buf[0] & 0xF0) >> 4, rx_buf[0] & 0x0F);
    }
    
    // Use expected size if valid, otherwise use full buffer
    size_t parse_size = (expected_size > 0 && expected_size <= transfer_size) ? expected_size : transfer_size;
    // Validate and parse the unified packet
    if (!validate_wave_packet(rx_buf, parse_size)) {
        if (s_hw) s_hw->PrintLine("DAISY: Invalid packet received - CRC validation failed");
        return false;
    }

    // Parse packet details
    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[2048]; // Buffer for payload data
    size_t payload_size;
    
    if (!parse_wave_packet(rx_buf, parse_size, msg_type, payload, payload_size, sequence_number, flags)) {
        if (s_hw) s_hw->PrintLine("DAISY: Failed to parse packet");
        return false;
    }

    // Check for duplicate packets using sequence numbers (packet-level concern)
    if (is_duplicate_packet(sequence_number)) {
        if (s_hw) s_hw->PrintLine("DAISY: Dropping duplicate/out-of-order packet: seq=%u", sequence_number);
        return true; // Successfully handled (by ignoring)
    }

    // Handle packet-level flags (ACK/NACK) here
    if (flags & PKT_FLAG_ACK) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Received ACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        }
        // Handle acknowledgment - remove from retry queue if needed
        return true;
    }
        
    if (flags & PKT_FLAG_NACK) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Received NACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        }
        // Handle negative acknowledgment - retry if needed
        return true;
    }

    // Route message by type - pass only the payload (packet processing is complete)
    ProcessSpiMessageByType(msg_type, sequence_number, payload, payload_size);
    return true;
}

// Perform bidirectional poll - sends queued outgoing packets (non-blocking)
static bool PerformBidirectionalPoll()
{
    // Send outgoing packets if we have any (non-blocking)
    if (outgoing_count > 0) {
        uint8_t* outgoing_msg = outgoing_queue[outgoing_head];
        size_t packet_size = get_packet_size(outgoing_msg);
        
        // Send the packet (non-blocking DMA)
        daisy::SpiHandle::Result result = Spi_SendPacket(outgoing_msg, packet_size);
        
        if (result == daisy::SpiHandle::Result::OK) {
            // Successfully sent, remove from queue
            outgoing_head = (outgoing_head + 1) % MAX_QUEUED_MESSAGES;
            outgoing_count--;
            s_stats.packets_sent++;
            
            if (s_hw) {
                uint8_t msg_type = outgoing_msg[1];
                uint16_t seq_num = outgoing_msg[2] | (outgoing_msg[3] << 8);
                s_hw->PrintLine("DAISY: Sent packet type=0x%02X, seq=%u, size=%d, remaining=%d", 
                               msg_type, seq_num, (int)packet_size, outgoing_count);
            }
            return true;
        } else {
            if (s_hw) {
                s_hw->PrintLine("DAISY: Failed to send packet, result=%d", (int)result);
            }
            return false;
        }
    }
    
    // No outgoing packets to send
    return true;
}


// ============================================================================
// Public API Implementation
// ============================================================================

bool Spi_SendPreCreatedPacket(const uint8_t* packet_data, size_t packet_size)
{
    if (packet_data == NULL || packet_size == 0 || packet_size > MAX_PKT_SIZE) {
        return false;
    }
    
    return QueueOutgoingMessage(packet_data, packet_size);
}

void Spi_GetStats(spi_link_stats_t* stats)
{
    if (stats) {
        memcpy(stats, &s_stats, sizeof(s_stats));
    }
}

void Spi_DebugState()
{
    if (s_hw) {
        s_hw->PrintLine("DAISY: SPI Debug State");
        s_hw->PrintLine("  Incoming Queue: head=%d, tail=%d, count=%d", queue_head, queue_tail, queue_count);
        s_hw->PrintLine("  Outgoing Queue: head=%d, tail=%d, count=%d", outgoing_head, outgoing_tail, outgoing_count);
#if WAVEX_SPI_DMA_ENABLED
        s_hw->PrintLine("  DMA RX Buffers: current=%u, ready=%u", 
                        (unsigned)s_current_rx_buffer, (unsigned)s_packets_ready_for_processing);
        s_hw->PrintLine("  RX Buffer Status: [%u,%u,%u,%u]", 
                        (unsigned)s_ready_rx_buffers[0], (unsigned)s_ready_rx_buffers[1],
                        (unsigned)s_ready_rx_buffers[2], (unsigned)s_ready_rx_buffers[3]);
        s_hw->PrintLine("  TX Buffer Status: [%u,%u,%u,%u]", 
                        (unsigned)s_tx_buffer_states[0], (unsigned)s_tx_buffer_states[1],
                        (unsigned)s_tx_buffer_states[2], (unsigned)s_tx_buffer_states[3]);
        s_hw->PrintLine("  DMA State: tx_inflight=%s, duplex_inflight=%s", 
                        s_tx_inflight ? "true" : "false", 
                        s_duplex_inflight ? "true" : "false");
#endif
        s_hw->PrintLine("  Stats: sent=%lu, received=%lu, crc_errors=%lu, rx_overflows=%lu",
                        (unsigned long)s_stats.packets_sent,
                        (unsigned long)s_stats.packets_received, 
                        (unsigned long)s_stats.crc_errors,
                        (unsigned long)s_stats.rx_q_overflows);
    }
}

void Spi_CheckTimeout()
{
#if WAVEX_SPI_DMA_ENABLED
    if (s_tx_inflight) {
        uint32_t current_time = System::GetTick();
        uint32_t elapsed = current_time - s_dma_start_time;
        
        if (elapsed > DMA_TIMEOUT_MS) {
            if (s_hw) s_hw->PrintLine("DAISY: DMA timeout after %u ms - forcing abort", elapsed);
            if (s_hw) s_hw->PrintLine("DAISY: Timeout details: start_time=%u, current_time=%u, inflight=%s", 
                                     s_dma_start_time, current_time, s_tx_inflight ? "true" : "false");
            
            // Force CS high and clear inflight flag
            cs_pin.Write(true);
            s_tx_inflight = false;
            
            if (s_hw) s_hw->PrintLine("DAISY: DMA timeout recovery - CS deasserted, ready for next packet");
        }
    }
#endif
}

// Process queued SPI messages (public API function) - now with batch processing
void ProcessQueuedSpiMessage()
{
    const int MAX_PACKETS_PER_CALL = 4; // Process up to 4 packets per main loop call
    int processed_count = 0;
    
    // Process DMA packets - copy from DMA buffer to storage and process
#if WAVEX_SPI_DMA_ENABLED
    while (s_packets_ready_for_processing > 0 && processed_count < MAX_PACKETS_PER_CALL) {
        // Find a free storage buffer
        uint32_t storage_buffer = 0;
        bool found_free = false;
        for (uint32_t i = 0; i < RX_BUFFER_COUNT; i++) {
            if (s_ready_rx_buffers[i] == 0) { // Free
                storage_buffer = i;
                found_free = true;
                break;
            }
        }
        
        if (!found_free) {
            if (s_hw) s_hw->PrintLine("DAISY: No free RX storage buffers - dropping packet");
            s_packets_ready_for_processing--;
            continue;
        }
        
        // Copy from DMA buffer to storage buffer (main loop context - safe!)
        memcpy(s_rx_buffers[storage_buffer], s_rx_dma_buf, MAX_PKT_SIZE);
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Processing DMA packet (remaining: %u)", s_packets_ready_for_processing);
        }
        
        // Process the packet from storage buffer
        ProcessReceivedPacket(s_rx_buffers[storage_buffer], MAX_PKT_SIZE);
        
        // Decrement counter
        s_packets_ready_for_processing--;
        processed_count++;
    }
#endif

    // Process multiple outgoing packets - send up to MAX_PACKETS_PER_CALL
    while (outgoing_count > 0 && processed_count < MAX_PACKETS_PER_CALL) {
        // Send one packet from the queue
        uint8_t* outgoing_msg = outgoing_queue[outgoing_head];
        size_t packet_size = get_packet_size(outgoing_msg);
        
        // Send the packet (non-blocking DMA)
        daisy::SpiHandle::Result result = Spi_SendPacket(outgoing_msg, packet_size);
        
        if (result == daisy::SpiHandle::Result::OK) {
            // Successfully sent, remove from queue
            outgoing_head = (outgoing_head + 1) % MAX_QUEUED_MESSAGES;
            outgoing_count--;
            
            if (s_hw) {
                uint8_t msg_type = outgoing_msg[1];
                uint16_t seq_num = outgoing_msg[2] | (outgoing_msg[3] << 8);
                s_hw->PrintLine("DAISY: Sent packet type=0x%02X, seq=%u, size=%d, remaining=%d", 
                               msg_type, seq_num, (int)packet_size, outgoing_count);
            }
            processed_count++;
        } else {
            if (s_hw) s_hw->PrintLine("DAISY: Send failed with result %d - stopping batch", (int)result);
            break; // Stop on send error
        }
    }

    // Process multiple queued messages (if we start using the incoming message queue)
    while (queue_count > 0 && processed_count < MAX_PACKETS_PER_CALL) {
        uint8_t* packet_data = message_queue[queue_head];
        size_t packet_size = get_packet_size(packet_data);
        
        // Extract packet information using new unified format
        uint8_t msg_type = packet_data[1];
        uint8_t flags = packet_data[0] & PKT_FLAG_MASK;
        uint16_t sequence_number = packet_data[2] | (packet_data[3] << 8);

        WAVEX_LOG_DAISY_INBOUND(DAISY_INBOUND_SPI, "Dequeuing message for processing, msg_type=0x%02X, flags=0x%02X, seq=%u, size=%d",
                           msg_type, flags, sequence_number, (int)packet_size);

        // Handle ACK/NACK flags here
        if (flags & PKT_FLAG_ACK) {
            if (s_hw) {
                s_hw->PrintLine("DAISY: Queue - Received ACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
            }
        } else if (flags & PKT_FLAG_NACK) {
            if (s_hw) {
                s_hw->PrintLine("DAISY: Queue - Received NACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
            }
        } else {
            // Extract payload and process message
            size_t payload_size = packet_size - 6; // header(4) + crc(2)
            const uint8_t* payload = packet_data + 4;
            
            ProcessSpiMessageByType(msg_type, sequence_number, payload, payload_size);
        }
        
        // Dequeue
        queue_head = (queue_head + 1) % MAX_QUEUED_MESSAGES;
        queue_count--;
        processed_count++;
        
        WAVEX_LOG_DAISY_INBOUND(DAISY_INBOUND_SPI, "Message processed and dequeued, remaining queue_count=%d", queue_count);
    }
    
    // Log batch processing stats periodically
    static uint32_t s_last_batch_log = 0;
    if (processed_count > 1 && (System::GetTick() - s_last_batch_log) > 1000) {
        s_last_batch_log = System::GetTick();
        if (s_hw) s_hw->PrintLine("DAISY: Batch processed %d packets in single call", processed_count);
    }
}

// ============================================================================
// Non-blocking DMA duplex transaction to receive data from ESP32 (outside namespace)
// ============================================================================
daisy::SpiHandle::Result Spi_ReceivePacket()
{
    if (!g_spi_handle) {
        if (s_hw) s_hw->PrintLine("Spi_ReceivePacket ERROR: g_spi_handle is NULL!");
        return daisy::SpiHandle::Result::ERR;
    }

#if WAVEX_SPI_DMA_ENABLED
    if (s_duplex_inflight) {
        if (s_hw) s_hw->PrintLine("Spi_ReceivePacket: Duplex transaction already in flight");
        return daisy::SpiHandle::Result::ERR;
    }

    // Find next available RX buffer (check for buffer overflow)
    // Clear the RX DMA buffer
    memset(s_rx_dma_buf, 0, MAX_PKT_SIZE);
    
    // Prepare TX buffer with outgoing data if available, otherwise zeros
    WaveX::Comm::PrepareTxBuffer(s_tx_dma_buf, MAX_PKT_SIZE);
    
    // Cache operations removed - buffers are in non-cacheable DMA memory

    if (s_hw) s_hw->PrintLine("DAISY: Starting DMA duplex transaction to receive from ESP32");

    // Start DMA duplex transfer (bidirectional); CS low/high handled in callbacks
    // cs_pin.Write(false);
    daisy::SpiHandle::Result dma_result = g_spi_handle->DmaTransmitAndReceive(
        s_tx_dma_buf,
        s_rx_dma_buf, // Use DMA buffer
        MAX_PKT_SIZE,
        spi_duplex_start_cb,
        spi_duplex_end_cb,
        NULL);
    // cs_pin.Write(true);

    if (s_hw) s_hw->PrintLine("DAISY: DmaTransmitAndReceive returned: %d", (int)dma_result);
    
    return dma_result;
#else
    // Fallback: blocking duplex with software CS
    if (s_hw) s_hw->PrintLine("DAISY: Starting blocking duplex transaction");
    
    uint8_t tx_buf[MAX_PKT_SIZE];
    uint8_t rx_buf[MAX_PKT_SIZE];
    
    WaveX::Comm::PrepareTxBuffer(tx_buf, MAX_PKT_SIZE);
    memset(rx_buf, 0, MAX_PKT_SIZE);
    
    // Wait longer for ESP32 to be ready
    System::DelayUs(ESP32_DMA_READY_WAIT_MS));

    // Check if ESP32 is still signaling data ready
    if (!attn_pin.Read()) {
        if (s_hw) s_hw->PrintLine("DAISY: ESP32 attention signal lost, aborting transaction");
        return daisy::SpiHandle::Result::ERR;
    }

    // Try a different approach - use smaller packet size first with retry
    if (s_hw) s_hw->PrintLine("DAISY: Starting transaction with smaller packet size");
    daisy::SpiHandle::Result res = daisy::SpiHandle::Result::ERR;

    // Retry up to 3 times for reliability
    for (int attempt = 0; attempt < 3; attempt++) {
        cs_pin.Write(false);
        System::DelayUs(10); // Small delay for setup

        // Try with a smaller packet size first (32 bytes)
        res = g_spi_handle->BlockingTransmitAndReceive(tx_buf, rx_buf, 32, 100);

        cs_pin.Write(true); // Always deassert CS

        if (res == daisy::SpiHandle::Result::OK) {
            if (s_hw) s_hw->PrintLine("DAISY: Transaction successful on attempt %d", attempt + 1);
            break;
        } else {
            if (s_hw) s_hw->PrintLine("DAISY: Transaction failed on attempt %d, result=%d", attempt + 1, (int)res);
            System::DelayUs(100); // Small delay between retries
        }
    }

    // Simple success/failure check
    if (res != daisy::SpiHandle::Result::OK) {
        if (s_hw) s_hw->PrintLine("DAISY: Small packet transaction failed with result: %d", (int)res);
        
        // Fallback: just signal completion without data exchange
        if (s_hw) s_hw->PrintLine("DAISY: Using fallback - no data exchange");
        res = daisy::SpiHandle::Result::OK; // Mark as successful to continue
    } else {
        if (s_hw) s_hw->PrintLine("DAISY: Small packet transaction completed successfully");
    }
    
    // Process the received data if transaction was successful
    if (res == daisy::SpiHandle::Result::OK) {
        // Process received packet with actual size used
        ProcessReceivedPacket(rx_buf, 32);
    }
    
    // TODO: DELETE ME
    // // Clear the attention signal by waiting for it to go low
    // int clear_attempts = 0;
    // while (attn_pin.Read() && clear_attempts < 10) {
    //     System::DelayUs(100);
    //     clear_attempts++;
    // }
    
    // if (s_hw) s_hw->PrintLine("DAISY: Attention signal cleared after %d attempts", clear_attempts);
    
    
    return res;
#endif
}

} // namespace Comm
} // namespace WaveX


// ============================================================================
// ATTN Pin Interrupt Handler (outside namespace)
// ============================================================================

static volatile bool s_interrupt_processing = false;
extern "C" void EXTI15_10_IRQHandler(void)
{
    // Check if this interrupt is from our attention pin (D0/PB12)
    if (__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_12) != RESET) {
        // Clear the interrupt flag
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_12);
        
        // Check if attention pin is high (ESP32 signaling data ready)
        if (attn_pin.Read() && !s_interrupt_processing) {
            if (s_hw) s_hw->PrintLine("DAISY: ATTN interrupt - ESP32 has data ready");
            
            // Initiate non-blocking DMA duplex transaction to receive data
            s_interrupt_processing = true;
            WaveX::Comm::Spi_ReceivePacket();
            s_interrupt_processing = false;
        }
    }
}

#endif // WAVEX_SPI_LINK_ENABLED

