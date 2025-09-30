#include "daisy_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

// Force platform define for linter
#ifndef DAISY_PLATFORM
#define DAISY_PLATFORM 1
#endif

#include "daisy_seed.h"
#include <string.h>
#include <stdint.h>
#include <cstdlib>
#include "per/gpio.h"
#include "per/spi.h"
#include "sys/system.h" // For System::DelayUs
#include "sys/dma.h"    // For DMA cache helpers
#include "daisy_core.h" // For DMA_BUFFER_MEM_SECTION
#include "stm32h7xx_hal.h" // For SCB cache maintenance functions and STM32 HAL GPIO and interrupt functions
#include "stm32h7xx_hal_cortex.h" // For SCB cache maintenance functions
#include "../../shared/config/link_config.h" // For HD protocol command macros
#include "../../shared/config/pin_config.h" // For Daisy SPI pin definitions
#include "../../shared/spi_protocol/protocol.h" // For protocol functions
#include "../../shared/config/logging_config.h" // For logging macros
#include "../storage/fs_browse.h" // For file browsing
#include "../audio/audio_engine.h" // For sample audition

using namespace daisy;
using namespace WaveX::Protocol;

// Stub FileSystem namespace for compilation
namespace WaveX {
namespace Storage {
class FileSystem {
public:
    static bool GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len);
};
} // namespace Storage
} // namespace WaveX

// Forward declarations for message processing functions
static void ProcessSyncMessage(const uint8_t* payload, size_t payload_size);
static void ProcessControlChangeMessage(const uint8_t* payload, size_t payload_size);
static void ProcessNoteMessage(const uint8_t* payload, size_t payload_size);
static void ProcessNoteOffMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSampleLoadMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSampleControlMessage(const uint8_t* payload, size_t payload_size);
static void ProcessPreviewRequestMessage(const uint8_t* payload, size_t payload_size);
static void ProcessDataRequestMessage(const uint8_t* payload, size_t payload_size);
static void ProcessMeterPushMessage(const uint8_t* payload, size_t payload_size);
static void ProcessWaveChunkMessage(const uint8_t* payload, size_t payload_size);
static void ProcessHeartbeatMessage(const uint8_t* payload, size_t payload_size);
static void ProcessBrowseResponseMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSampleStopRequestMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSampleStatusMessage(const uint8_t* payload, size_t payload_size);

static void ProcessSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size);
static void ProcessSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size);
static void ProcessAckMessage(const uint8_t* payload, size_t payload_size);
static void ProcessErrorMessage(const uint8_t* payload, size_t payload_size);

// Use flexible packet structures
#define CMD_PKT_SIZE PKT_SIZE_32
#define DATA_PKT_SIZE PKT_SIZE_1024  // Increased from 256 to 1024 bytes
#define MAX_PKT_SIZE 2048  // Support up to 2KB packets

// Fixed-size packet typedefs removed - using flexible packet system only

// Legacy compatibility - use command packet size as default
#define CTRL_PKT_SIZE CMD_PKT_SIZE

// Use shared CRC function
#define crc16_ccitt ProtocolHandler::CalculateSpiCrc

// Use new unified packet system functions
#define get_packet_size_from_code ProtocolHandler::GetPacketSizeFromCode
#define get_optimal_size_code ProtocolHandler::GetOptimalSizeCode
#define calculate_wave_crc ProtocolHandler::CalculateWaveXCrc
#define validate_wave_packet ProtocolHandler::ValidateWaveXPacket
#define create_wave_packet ProtocolHandler::CreateWaveXPacket
#define parse_wave_packet ProtocolHandler::ParseWaveXPacket

// Legacy function compatibility (for functions that were removed)
#define validate_packet validate_wave_packet

// Compatibility function for get_packet_size - extracts size code from packet data
static size_t get_packet_size(const uint8_t* packet_data) {
    if (!packet_data) return 0;
    uint8_t size_code = packet_data[0] & PKT_SIZE_MASK;
    return get_packet_size_from_code(size_code);
}

// ============================================================================
// Module-level static variables
// ============================================================================

static daisy::DaisySeed* s_hw = NULL;
static daisy::SpiHandle* g_spi_handle = NULL;
static daisy::GPIO cs_pin;
static daisy::GPIO attn_pin;
static WaveX::Comm::spi_link_stats_t s_stats = {};

static volatile bool g_esp32_attention_flag = false; // Flag for ESP32 attention

// Message queue for received messages (unified for both packet types)
#define MAX_QUEUED_MESSAGES 8

// Outgoing message queue (Daisy → ESP32)

// Directory state for file browsing
static char s_current_directory[96] = "/";
static WaveX::Storage::FileEntry s_current_file_entries[50]; // Increased to accommodate more files
static size_t s_current_file_count = 0;
static bool s_directory_state_valid = false;

// Message deduplication - simple approach for single-threaded execution
static uint8_t s_last_processed_packet[32] = {0};
static bool s_has_last_packet = false;

// Implementation of FileSystem::GetFilePathByIndex
namespace WaveX {
namespace Storage {
bool FileSystem::GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len) {
    // Use cached directory state to get file path by index
    if (!s_directory_state_valid || file_index >= s_current_file_count) {
        return false; // No valid directory state or index out of range
    }
    
    const WaveX::Storage::FileEntry& entry = s_current_file_entries[file_index];
    
    // Build full path: current_directory + "/" + filename
    size_t dir_len = strlen(s_current_directory);
    size_t name_len = strlen(entry.name);
    
    // Check if we have enough space
    if (dir_len + 1 + name_len + 1 > max_len) {
        return false; // Path too long
    }
    
    // Build the path
    strcpy(file_path, s_current_directory);
    
    // Add "/" if current directory is not root and doesn't end with "/"
    if (strcmp(s_current_directory, "/") != 0 && s_current_directory[dir_len - 1] != '/') {
        strcat(file_path, "/");
    }
    
    strcat(file_path, entry.name);
    
    return true;
}
} // namespace Storage
} // namespace WaveX

// Forward declarations will be inside namespace

#if WAVEX_SPI_DMA_ENABLED
static volatile bool s_tx_inflight = false;
static volatile bool s_duplex_inflight = false;
static volatile bool s_packet_ready_for_processing = false;
static uint32_t s_dma_start_time = 0;
static const uint32_t DMA_TIMEOUT_MS = 1000; // 1 second timeout
// DMA buffers in non-cacheable SRAM for proper coherency
// Use MAX_PKT_SIZE to support all packet sizes up to 4KB
static uint8_t s_tx_dma_buf[MAX_PKT_SIZE] DMA_BUFFER_MEM_SECTION;
static uint8_t s_rx_dma_buf[MAX_PKT_SIZE] DMA_BUFFER_MEM_SECTION;

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
    s_dma_start_time = System::GetTick();
    if (s_hw) s_hw->PrintLine("DAISY: === DMA START CALLBACK FIRED === CS asserted, start_time=%u", s_dma_start_time);
    if (s_hw) s_hw->PrintLine("DAISY: DMA start callback fired - transaction is actually running");
    
    // Add timeout check - if DMA doesn't complete within timeout, force abort
    // This will be checked in the main loop
}

static void spi_dma_end_cb(void* /*context*/, daisy::SpiHandle::Result result)
{
    // Critical debug: Verify this callback actually fires
    if (s_hw) s_hw->PrintLine("DAISY: === DMA END CALLBACK FIRED === result=%d", (int)result);
    
    // GPIO toggle for hardware verification (in case logging is buffered)
    static bool toggle_state = false;
    toggle_state = !toggle_state;
    // Toggle a pin to verify IRQ is firing (can be observed with oscilloscope)
    // Using built-in LED or a spare GPIO pin would work here
    
    // Cache operations removed - buffers are in non-cacheable DMA memory
    
    // Deassert CS and update state
    cs_pin.Write(true);
    uint32_t duration = System::GetTick() - s_dma_start_time;
    if(result == daisy::SpiHandle::Result::OK)
    {
        s_stats.packets_sent++;
        if (s_hw) s_hw->PrintLine("DAISY: DMA transaction completed successfully, duration=%u ms", duration);
        if (s_hw) s_hw->PrintLine("DAISY: RX buffer contents: %02X %02X %02X %02X %02X %02X %02X %02X", 
                                   s_rx_dma_buf[0], s_rx_dma_buf[1], s_rx_dma_buf[2], s_rx_dma_buf[3],
                                   s_rx_dma_buf[4], s_rx_dma_buf[5], s_rx_dma_buf[6], s_rx_dma_buf[7]);
    } else {
        if (s_hw) s_hw->PrintLine("DAISY: DMA transaction failed with result: %d, duration=%u ms", (int)result, duration);
    }
    s_tx_inflight = false;
    if (s_hw) s_hw->PrintLine("DAISY: s_tx_inflight cleared, ready for next packet");
}

static void spi_duplex_start_cb(void* /*context*/)
{
    // Assert CS at the moment DMA actually starts
    cs_pin.Write(false);
}

static void spi_duplex_end_cb(void* /*context*/, daisy::SpiHandle::Result result)
{
    // Deassert CS and update state
    cs_pin.Write(true);
    if(result == daisy::SpiHandle::Result::OK)
    {
        s_stats.packets_received++;
        // Signal that packet is ready for processing
        s_packet_ready_for_processing = true;
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

    // Copy payload into persistent DMA buffer
    memcpy(s_tx_dma_buf, tx_buf, packet_size);
    
    // Clear RX buffer before transaction
    memset(s_rx_dma_buf, 0, packet_size);
    
    // Cache operations removed - buffers are in non-cacheable DMA memory

    s_tx_inflight = true;
    if (s_hw) s_hw->PrintLine("DAISY: Starting DMA duplex transaction, packet_size=%d", (int)packet_size);
    if (s_hw) s_hw->PrintLine("DAISY: TX buffer contents: %02X %02X %02X %02X %02X %02X %02X %02X", 
                               s_tx_dma_buf[0], s_tx_dma_buf[1], s_tx_dma_buf[2], s_tx_dma_buf[3],
                               s_tx_dma_buf[4], s_tx_dma_buf[5], s_tx_dma_buf[6], s_tx_dma_buf[7]);
    
    // Start DMA duplex transfer
    uint32_t call_start_time = System::GetTick();
    daisy::SpiHandle::Result dma_result = g_spi_handle->DmaTransmitAndReceive(
        s_tx_dma_buf,
        s_rx_dma_buf,
        packet_size,
        spi_dma_start_cb,
        spi_dma_end_cb,
        NULL);
    
    uint32_t call_end_time = System::GetTick();
    uint32_t call_duration = call_end_time - call_start_time;
    
    if (s_hw) s_hw->PrintLine("DAISY: DmaTransmitAndReceive call completed at time=%u, duration=%u ms", call_end_time, call_duration);
    if (s_hw) s_hw->PrintLine("DAISY: DmaTransmitAndReceive returned: %d", (int)dma_result);
    
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
        
        // Fallback to blocking call since DMA is broken
        if (s_hw) s_hw->PrintLine("DAISY: DMA failed - using blocking fallback...");
        uint8_t dummy_rx[32];
        cs_pin.Write(false);
        uint32_t blocking_start = System::GetTick();
        daisy::SpiHandle::Result blocking_result = g_spi_handle->BlockingTransmitAndReceive(
            s_tx_dma_buf, dummy_rx, packet_size, 1000);
        uint32_t blocking_duration = System::GetTick() - blocking_start;
        cs_pin.Write(true);
        
        if (blocking_result == daisy::SpiHandle::Result::OK) {
            s_stats.packets_sent++;
            if (s_hw) s_hw->PrintLine("DAISY: Blocking call SUCCESS: duration=%u ms", blocking_duration);
            return blocking_result; // Return success for blocking call
        } else {
            if (s_hw) s_hw->PrintLine("DAISY: Blocking call FAILED: result=%d, duration=%u ms", (int)blocking_result, blocking_duration);
        }
    } else {
        // DMA started successfully - callback will handle completion
        if (s_hw) s_hw->PrintLine("DAISY: DMA transaction started successfully, waiting for completion callback...");
    }
    
    // Add small delay to prevent overwhelming ESP32
    System::DelayUs(500); // Reduced from 1000us to 100us for better performance
    
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

// Legacy compatibility function
static daisy::SpiHandle::Result Spi_SendRaw64(const uint8_t* tx_buf)
{
    return Spi_SendPacket(tx_buf, CMD_PKT_SIZE);
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

// Forward declarations
static void ProcessBrowseRequestMessage(const uint8_t* packet_data, size_t packet_size);

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

// Forward declarations for outgoing message queue functions
static bool QueueOutgoingMessage(const uint8_t* packet_data, size_t packet_size);
static bool Spi_HasOutgoingData();
static void PrepareTxBuffer(uint8_t* tx_buf, size_t buf_size);
static bool PerformBidirectionalPoll();
static bool ProcessReceivedPacket(const uint8_t* rx_buf, size_t transfer_size);
// Forward declaration removed - function is now public

void Spi_Init(daisy::DaisySeed &hw, daisy::SpiHandle* hspi)
{
    s_hw = &hw;
    hw.PrintLine("SPI Init: Daisy Master for ESP32 Slave");
    hw.PrintLine("Spi_Init called with hspi=%p", hspi);
    
    if (!hspi) {
        hw.PrintLine("ERROR: SPI handle is null.");
        return;
    }

    // 1) Claim CS as a plain GPIO *before* SPI init
    daisy::Pin cs_p = hw.GetPin(WAVEX_DAISY_SPI_CS);
    cs_pin.Init(cs_p, daisy::GPIO::Mode::OUTPUT, daisy::GPIO::Pull::PULLUP, daisy::GPIO::Speed::VERY_HIGH);
    cs_pin.Write(true); // Start with CS high (inactive)

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
    
    // Configure interrupt priorities to prevent audio starvation
    // Audio DMA should have higher priority (lower number) than SPI
    // This will be handled in the main audio initialization
    
    spi_config.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    spi_config.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    spi_config.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    spi_config.pin_config.nss  = Pin(); // IMPORTANT: leave unassigned for software CS
    
    hw.PrintLine("Spi_Init: About to call hspi->Init");
    SpiHandle::Result result = hspi->Init(spi_config);
    hw.PrintLine("Spi_Init: hspi->Init returned: %d", (int)result);

    if (result == SpiHandle::Result::OK) {
        hw.PrintLine("SUCCESS: SPI master configured correctly!");
        g_spi_handle = hspi;
        hw.PrintLine("Spi_Init: g_spi_handle set to %p", g_spi_handle);
        memset(&s_stats, 0, sizeof(s_stats));
        hw.PrintLine("Spi_Init: Stats initialized");
    } else {
        hw.PrintLine("ERROR: SPI init failed with result: %d", (int)result);
        g_spi_handle = NULL;
        hw.PrintLine("Spi_Init: g_spi_handle set to NULL");
    }
}

// ============================================================================
// ESP32 Message Processing
// ============================================================================

// Forward declarations for message processing functions
static void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries = 20);
static void ProcessSamplePlayRequest(const char* file_path);
static void ProcessSampleStopRequest();
static void ProcessSamplePlayIndexRequest(uint32_t file_index);
static void ProcessSampleGetPathRequest(uint32_t file_index);
static void ClearDirectoryState();

static void ProcessSpiMessageByType(uint8_t msg_type, uint16_t sequence_number, const uint8_t* payload, size_t payload_size)
{
    using namespace WaveX::Protocol;

    // Debug: Log message processing
    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing message - msg_type=0x%02X, seq=%u, payload_size=%d bytes",
                        msg_type, sequence_number, (int)payload_size);
        if (payload_size > 0) {
            s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                            payload[0], payload[1], payload[2], payload[3],
                            payload[4], payload[5], payload[6], payload[7]);
        }
    }

    // Route based on message type - all handlers now receive payload only
    switch (msg_type) {
        case MSG_SYNC:
            ProcessSyncMessage(payload, payload_size);
            break;
        case MSG_CONTROL_CHANGE:
            ProcessControlChangeMessage(payload, payload_size);
            break;
        case MSG_NOTE_ON:
            ProcessNoteMessage(payload, payload_size);
            break;
        case MSG_NOTE_OFF:
            ProcessNoteOffMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_LOAD:
            ProcessSampleLoadMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_CTRL:
            ProcessSampleControlMessage(payload, payload_size);
            break;
        case MSG_PREVIEW_REQ:
            ProcessPreviewRequestMessage(payload, payload_size);
            break;
        case MSG_DATA_REQUEST:
            ProcessDataRequestMessage(payload, payload_size);
            break;
        case MSG_METER_PUSH:
            ProcessMeterPushMessage(payload, payload_size);
            break;
        case MSG_WAVE_CHUNK:
            ProcessWaveChunkMessage(payload, payload_size);
            break;
        case MSG_HEARTBEAT:
            ProcessHeartbeatMessage(payload, payload_size);
            break;
        case MSG_BROWSE_REQ:
            if (s_hw) s_hw->PrintLine("DAISY: Routing MSG_BROWSE_REQ to ProcessBrowseRequestMessage");
            ProcessBrowseRequestMessage(payload, payload_size);
            break;
        case MSG_BROWSE_RESP:
            ProcessBrowseResponseMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_REQ:
            ProcessSamplePlayRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STOP_REQ:
            ProcessSampleStopRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_STATUS:
            ProcessSampleStatusMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_PLAY_INDEX_REQ:
            ProcessSamplePlayIndexRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_REQ:
            ProcessSampleGetPathRequestMessage(payload, payload_size);
            break;
        case MSG_SAMPLE_GET_PATH_RESP:
            ProcessSampleGetPathResponseMessage(payload, payload_size);
            break;
        case MSG_ACK:
            ProcessAckMessage(payload, payload_size);
            break;
        case MSG_ERROR:
            ProcessErrorMessage(payload, payload_size);
            break;
        default:
            if (s_hw) {
                s_hw->PrintLine("DAISY: Unknown message type: 0x%02X", msg_type);
            }
            break;
    }
}

// Process browse request message
static void ProcessBrowseRequestMessage(const uint8_t* payload, size_t payload_size)
{
    using namespace WaveX::Protocol;

    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessBrowseRequestMessage called - payload_size=%d", (int)payload_size);
        s_hw->PrintLine("DAISY: Payload bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                       payload[0], payload[1], payload[2], payload[3],
                       payload[4], payload[5], payload[6], payload[7]);
    }

    // Parse browse request: start_index (1 byte) + path (null-terminated)
    uint8_t start_index = payload[0];
    const char* path_ptr = (const char*)(payload + 1);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Parsed start_index=%d, path_ptr='%s'", start_index, path_ptr);
    }
    
    char path[96] = {0};
    size_t path_len = strlen(path_ptr);
    if (path_len >= sizeof(path)) {
        path_len = sizeof(path) - 1;
    }
    memcpy(path, path_ptr, path_len);
    path[path_len] = '\0';
    
    uint8_t max_entries = 20; // Default to 20 entries

    if (s_hw) {
        s_hw->PrintLine("DAISY: Calling ProcessBrowseRequest with path='%s', start_index=%d, max_entries=%d", 
                       path, start_index, max_entries);
    }

    ProcessBrowseRequest(path, start_index, max_entries);
}





















// Stub implementations for all message processing functions

















// Process browse request (existing function - updated for new format)
static void ProcessBrowseRequest(const char* path, size_t start_index, uint8_t max_entries)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "IN MSG BROWSE_REQ path=%s start_index=%zu max_entries=%u", 
                           path, start_index, max_entries);

    // Allocate buffer for file entries
    FileEntry entries[32]; // Max 32 entries per response
    size_t actual_max_entries = (max_entries > 32) ? 32 : max_entries;
    
    size_t total_count = 0;
    size_t entries_written = 0;
    
    // Get directory listing from FatFS
    bool success = ListDir(path, entries, actual_max_entries, total_count, start_index, entries_written);
    
    if (!success) {
        WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to list directory: %s", path);
        return;
    }
    
    WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Directory listing: total=%zu written=%zu", total_count, entries_written);
    
    // Cache the directory state for index-based lookups
    strncpy(s_current_directory, path, sizeof(s_current_directory) - 1);
    s_current_directory[sizeof(s_current_directory) - 1] = '\0';
    
    // Cache all entries (not just the paginated ones)
    if (start_index == 0) {
        // If this is the first page, cache all entries
        size_t all_entries_count = 0;
        FileEntry all_entries[50];
        ListDir(path, all_entries, 50, total_count, 0, all_entries_count);
        
        s_current_file_count = all_entries_count;
        for (size_t i = 0; i < all_entries_count && i < 50; i++) {
            s_current_file_entries[i] = all_entries[i];
        }
        s_directory_state_valid = true;
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Cached directory state: %zu entries from '%s'", all_entries_count, path);
        }
    }
    
    // Convert FileEntry to FileEntryWire for transmission
    FileEntryWire wire_entries[32];
    for (size_t i = 0; i < entries_written; i++) {
        wire_entries[i].is_dir = entries[i].is_dir;
        wire_entries[i].size_bytes = entries[i].size_bytes;
        strncpy(wire_entries[i].name, entries[i].name, sizeof(wire_entries[i].name) - 1);
        wire_entries[i].name[sizeof(wire_entries[i].name) - 1] = '\0';
    }
    
    // Create browse response packet
    uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = ProtocolHandler::CreateBrowseRespPacket(response_buffer, sizeof(response_buffer),
                                                           total_count, wire_entries, entries_written);
    
    if (pkt_size > 0) {
        // Send response back to ESP32
        if (s_hw) s_hw->PrintLine("DAISY: Creating browse response packet, size=%d, entries=%zu", (int)pkt_size, entries_written);
        int result = Spi_SendPreCreatedPacket(response_buffer, pkt_size);
        if (result) {
            WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Sent browse response: %zu entries", entries_written);
            if (s_hw) s_hw->PrintLine("DAISY: Browse response queued successfully, outgoing_count=%d", outgoing_count);
        } else {
            WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to send browse response");
            if (s_hw) s_hw->PrintLine("DAISY: Failed to queue browse response");
        }
    } else {
        WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to create browse response packet");
        if (s_hw) s_hw->PrintLine("DAISY: Failed to create browse response packet");
    }
}

// Process sample play request (existing function)
static void ProcessSamplePlayRequest(const char* file_path)
{
    using namespace WaveX::Protocol;

    // ESP32 requesting sample audition
    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing MSG_SAMPLE_PLAY_REQ for file: '%s'", file_path);
    }

    // Start sample audition
    bool success = WaveX::AudioEngine::AuditionSample(file_path);

    // Send status response
    if (success) {
        WaveX::Protocol::SampleStatusMessage status;
        status.state = 1; // playing
        status.sample_rate = 44100; // TODO: get actual sample rate
        status.channels = 2; // TODO: get actual channels
        status.frames_played = 0;

        uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateSampleStatusPacket(response_buffer, sizeof(response_buffer), status);
    Spi_SendPreCreatedPacket(response_buffer, pkt_size);
    }
}

// Process sample stop request (existing function)
static void ProcessSampleStopRequest()
{
    using namespace WaveX::Protocol;

    // ESP32 requesting to stop sample audition
    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing MSG_SAMPLE_STOP_REQ");
    }

    // Stop sample audition
    WaveX::AudioEngine::StopAudition();

    // Send status response
    WaveX::Protocol::SampleStatusMessage status;
    status.state = 0; // stopped
    status.sample_rate = 0;
    status.channels = 0;
    status.frames_played = 0;

    uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateSampleStatusPacket(response_buffer, sizeof(response_buffer), status);
    Spi_SendPreCreatedPacket(response_buffer, pkt_size);
}

// Process sample play index request (existing function)
static void ProcessSamplePlayIndexRequest(uint32_t file_index)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing sample play index request for index %lu", (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Playing sample at path: '%s'", file_path);
        }

        // Start sample audition
        bool success = WaveX::AudioEngine::AuditionSample(file_path);

        // Send status response
        WaveX::Protocol::SampleStatusMessage status;
        status.state = success ? 1 : 0; // playing or stopped
        status.sample_rate = 44100; // TODO: get actual sample rate
        status.channels = 2; // TODO: get actual channels
        status.frames_played = 0;
        
        uint8_t response_buffer[MAX_PKT_SIZE];
        size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateSampleStatusPacket(response_buffer, sizeof(response_buffer), status);
        Spi_SendPreCreatedPacket(response_buffer, pkt_size);
    } else {
    if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to get file path for index %lu", (unsigned long)file_index);
        }
    }
}

// Process sample get path request (existing function)
static void ProcessSampleGetPathRequest(uint32_t file_index)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    if (s_hw) {
        s_hw->PrintLine("DAISY: Processing sample get path request for index %lu", (unsigned long)file_index);
    }

    // Get file path for index
    char file_path[200] = {0};
    if (FileSystem::GetFilePathByIndex(file_index, file_path, sizeof(file_path))) {
        WaveX::Protocol::SamplePathResponseMessage response;
    response.index = file_index;
        strncpy(response.path, file_path, sizeof(response.path) - 1);
    response.path[sizeof(response.path) - 1] = '\0';
    
        uint8_t response_buffer[MAX_PKT_SIZE];
    size_t pkt_size = WaveX::Protocol::ProtocolHandler::CreateSamplePathResponsePacket(response_buffer, sizeof(response_buffer), response);
    Spi_SendPreCreatedPacket(response_buffer, pkt_size);
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to get file path for index %lu", (unsigned long)file_index);
        }
    }
}

// Clear directory state (existing function)
static void ClearDirectoryState()
{
    if (s_hw) {
        s_hw->PrintLine("DAISY: Directory state cleared");
    }
}

// Prepare TX buffer (existing function)
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
        size_t packet_size = WaveX::Protocol::ProtocolHandler::CreateWaveXPacket(
            tx_buf, buf_size, static_cast<WaveX::Protocol::MessageType>(msg_type),
            nullptr, 0, 0, PKT_FLAG_ACK  // ACK flag indicates this is an acknowledgment
        );
        
        if (packet_size > 0) {
            if (s_hw) s_hw->PrintLine("DAISY: Prepared no-data response packet: size=%d", (int)packet_size);
        } else {
            // Fallback to zeros if packet creation failed
            memset(tx_buf, 0, buf_size);
        }
    }
}

// Check if we have outgoing data (existing function)
bool Spi_HasOutgoingData()
{
    return outgoing_count > 0;
}

// Add message to outgoing queue (existing function)
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
        s_hw->PrintLine("  Queue: head=%d, tail=%d, count=%d", queue_head, queue_tail, queue_count);
        s_hw->PrintLine("  Outgoing: head=%d, tail=%d, count=%d", outgoing_head, outgoing_tail, outgoing_count);
        s_hw->PrintLine("  Stats: packets_sent=%lu, packets_received=%lu, crc_errors=%lu",
                        (unsigned long)s_stats.packets_sent,
                        (unsigned long)s_stats.packets_received, 
                        (unsigned long)s_stats.crc_errors);
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
            
            // Force CS high and clear inflight flag
            cs_pin.Write(true);
            s_tx_inflight = false;
            
            if (s_hw) s_hw->PrintLine("DAISY: DMA timeout recovery - CS deasserted, ready for next packet");
        }
    }
#endif
}


// Process received packet (static function)
static bool ProcessReceivedPacket(const uint8_t* rx_buf, size_t transfer_size)
{
    if (s_hw) {
        s_hw->PrintLine("DAISY: ProcessReceivedPacket called - transfer_size=%d", (int)transfer_size);
    }

    if (!rx_buf || transfer_size == 0) {
        if (s_hw) s_hw->PrintLine("DAISY: Invalid rx_buf or transfer_size=0");
        return false;
    }

    // Debug: Always show first 8 bytes of received data
    if (s_hw) {
        s_hw->PrintLine("DAISY: Raw RX data: %02X %02X %02X %02X %02X %02X %02X %02X", 
                       rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], 
                       rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
    }

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
        if (s_hw) s_hw->PrintLine("DAISY: Received all-zero packet - ignoring");
        return true; // Successfully ignored
    }

    if (s_hw) {
        s_hw->PrintLine("DAISY: Non-zero packet detected, proceeding with processing");
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
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Using parse_size=%d for validation", (int)parse_size);
    }

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

    // Route message by type - pass only the payload
    ProcessSpiMessageByType(msg_type, sequence_number, payload, payload_size);
    return true;
}


// Perform bidirectional poll (static function) - now non-blocking
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

    // Clear RX buffer before transaction
    memset(s_rx_dma_buf, 0, MAX_PKT_SIZE);
    
    // Prepare TX buffer with outgoing data if available, otherwise zeros
    WaveX::Comm::PrepareTxBuffer(s_tx_dma_buf, MAX_PKT_SIZE);
    
    // Cache operations removed - buffers are in non-cacheable DMA memory

    s_duplex_inflight = true;
    if (s_hw) s_hw->PrintLine("DAISY: Starting DMA duplex transaction to receive from ESP32");

    // Start DMA duplex transfer (bidirectional); CS low/high handled in callbacks
    daisy::SpiHandle::Result dma_result = g_spi_handle->DmaTransmitAndReceive(
        s_tx_dma_buf,
        s_rx_dma_buf,
        MAX_PKT_SIZE,
        spi_duplex_start_cb,
        spi_duplex_end_cb,
        NULL);

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
    System::DelayUs(500);

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

// Stub implementations for message processing functions (outside namespace)
static void ProcessSyncMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSyncMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessControlChangeMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessControlChangeMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessNoteMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessNoteMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessNoteOffMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessNoteOffMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessSampleLoadMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleLoadMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessSampleControlMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleControlMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessPreviewRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessPreviewRequestMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessDataRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessDataRequestMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessMeterPushMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessMeterPushMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessWaveChunkMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessWaveChunkMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessHeartbeatMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessHeartbeatMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessBrowseResponseMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessBrowseResponseMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessSamplePlayRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSamplePlayRequestMessage called - payload_size=%d", (int)payload_size);
    // Parse the payload to extract file path
    if (payload_size > 0) {
        const char* file_path = reinterpret_cast<const char*>(payload);
        WaveX::Comm::ProcessSamplePlayRequest(file_path);
    }
}

static void ProcessSampleStopRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleStopRequestMessage called - payload_size=%d", (int)payload_size);
    WaveX::Comm::ProcessSampleStopRequest();
}

static void ProcessSampleStatusMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleStatusMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessSamplePlayIndexRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSamplePlayIndexRequestMessage called - payload_size=%d", (int)payload_size);
    
    // Parse the SamplePlayIndexMessage structure
    if (payload_size >= sizeof(WaveX::Protocol::SamplePlayIndexMessage)) {
        const WaveX::Protocol::SamplePlayIndexMessage* msg = 
            reinterpret_cast<const WaveX::Protocol::SamplePlayIndexMessage*>(payload);
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Parsed sample play index request - index=%lu", (unsigned long)msg->index);
        }
        
        WaveX::Comm::ProcessSamplePlayIndexRequest(msg->index);
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid payload size for SamplePlayIndexMessage: %d (expected %d)", 
                           (int)payload_size, (int)sizeof(WaveX::Protocol::SamplePlayIndexMessage));
        }
    }
}

static void ProcessSampleGetPathRequestMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleGetPathRequestMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessSampleGetPathResponseMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleGetPathResponseMessage called - payload_size=%d", (int)payload_size);
}

static void ProcessAckMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessAckMessage called - payload_size=%d", (int)payload_size);

    // ACK messages typically have minimal payload - just log for now
    // The actual ACK processing (sequence numbers, etc.) is handled at the packet level
    // in ProcessReceivedPacket before we get here
    
    if (s_hw) s_hw->PrintLine("DAISY: ACK message payload processing completed");
}

static void ProcessErrorMessage(const uint8_t* payload, size_t payload_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessErrorMessage called - payload_size=%d", (int)payload_size);
}

// Process queued SPI messages (public API function)
void WaveX::Comm::ProcessQueuedSpiMessage()
{
    // Check for packet ready from DMA callback
#if WAVEX_SPI_DMA_ENABLED
    if (s_packet_ready_for_processing) {
        s_packet_ready_for_processing = false;
        // Process the packet received via DMA
        if (s_hw) {
            s_hw->PrintLine("DAISY: Processing DMA packet");
        }
        ProcessReceivedPacket(s_rx_dma_buf, MAX_PKT_SIZE);
        return; // Process one packet per call
    }
#endif

    // Poll for new messages
    PerformBidirectionalPoll();

    // Process one message from queue if available
    if (queue_count > 0) {
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
        
        WAVEX_LOG_DAISY_INBOUND(DAISY_INBOUND_SPI, "Message processed and dequeued, remaining queue_count=%d", queue_count);
    }
}

#endif // WAVEX_SPI_LINK_ENABLED

