#include "inter_spi.h"

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
#include "config/link_config.h" // For HD protocol command macros
#include "config/pin_config.h" // For Daisy SPI pin definitions
#include "../../shared/spi_protocol/protocol.h" // For protocol functions
#include "../../shared/config/logging_config.h" // For logging macros
#include "../storage/fs_browse.h" // For file browsing
#include "../audio/audio_engine.h" // For sample audition
#include "stm32h7xx_hal.h" // For interrupt handling

using namespace daisy;
using namespace WaveX::Protocol;

// Stub FileSystem namespace for compilation
namespace WaveX {
namespace Storage {
class FileSystem {
public:
    static bool GetFilePathByIndex(uint32_t file_index, char* file_path, size_t max_len) {
        // Stub implementation - return false for now
        return false;
    }
};
} // namespace Storage
} // namespace WaveX

// Forward declarations for message processing functions
static void ProcessSyncMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessControlChangeMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessNoteMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessNoteOffMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSampleLoadMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSampleControlMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessPreviewRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessDataRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessMeterPushMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessWaveChunkMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessHeartbeatMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessBrowseRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessBrowseResponseMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSamplePlayRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSampleStopRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSampleStatusMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSamplePlayIndexRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSampleGetPathRequestMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessSampleGetPathResponseMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessAckMessage(const uint8_t* packet_data, size_t packet_size);
static void ProcessErrorMessage(const uint8_t* packet_data, size_t packet_size);

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

// Forward declarations will be inside namespace

#if WAVEX_SPI_DMA_ENABLED
static volatile bool s_tx_inflight = false;
static volatile bool s_duplex_inflight = false;
static volatile bool s_packet_ready_for_processing = false;
// DMA buffers in non-cacheable SRAM for proper coherency
// Use MAX_PKT_SIZE to support all packet sizes up to 4KB
static uint8_t s_tx_dma_buf[MAX_PKT_SIZE] __attribute__((section(".dma_buffer"))) __attribute__((aligned(32)));
static uint8_t s_rx_dma_buf[MAX_PKT_SIZE] __attribute__((section(".dma_buffer"))) __attribute__((aligned(32)));

// Static packet buffer pool for large packets (replaces malloc/free)
static uint8_t s_large_packet_pool[4][MAX_PKT_SIZE] __attribute__((aligned(32)));
static bool s_pool_used[4] = {false, false, false, false};

// Polling backpressure - minimum interval between polls when idle
static uint32_t s_last_poll_time = 0;
static const uint32_t MIN_POLL_INTERVAL_MS = 1; // 1ms minimum between polls

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
        return daisy::SpiHandle::Result::ERR;
    }

    // Copy payload into persistent DMA buffer
    memcpy(s_tx_dma_buf, tx_buf, packet_size);
    
    // Ensure cache coherency for DMA on STM32H7
    // SCB_CleanDCache_by_Addr((uint32_t*)s_tx_dma_buf, packet_size);

    s_tx_inflight = true;

    // Start DMA transfer (one-way); CS low/high handled in callbacks
    daisy::SpiHandle::Result dma_result = g_spi_handle->DmaTransmit(
        s_tx_dma_buf,
        packet_size,
        spi_dma_start_cb,
        spi_dma_end_cb,
        NULL);

    return dma_result;
#else
    // Fallback: blocking transmit
    cs_pin.Write(false);
    System::DelayUs(10); // Small delay for setup

    daisy::SpiHandle::Result res = g_spi_handle->BlockingTransmit((uint8_t*)tx_buf, packet_size, 100);

    cs_pin.Write(true);
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

    // Enable interrupt for the attention pin (D0 is PB12, which uses EXTI15_10)
    // Priority must be lower than audio (higher number) to prevent preemption.
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 14, 0); // Priority 14, as per plan
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

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
    
    // Configure interrupt priorities to prevent audio starvation
    // Audio DMA should have higher priority (lower number) than SPI
    // This will be handled in the main audio initialization
    
    spi_config.pin_config.sclk = hw.GetPin(WAVEX_DAISY_SPI_SCK);
    spi_config.pin_config.mosi = hw.GetPin(WAVEX_DAISY_SPI_MOSI);
    spi_config.pin_config.miso = hw.GetPin(WAVEX_DAISY_SPI_MISO);
    spi_config.pin_config.nss  = Pin(); // IMPORTANT: leave unassigned
    
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

static void ProcessSpiMessageByType(uint8_t msg_type, uint8_t flags, uint16_t sequence_number, const uint8_t* packet_data, size_t packet_size)
{
    using namespace WaveX::Protocol;

    // Debug: Log received packet bytes
    if (s_hw) {
        s_hw->PrintLine("DAISY: Received packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                        packet_data[0], packet_data[1], packet_data[2], packet_data[3],
                        packet_data[4], packet_data[5], packet_data[6], packet_data[7]);
        s_hw->PrintLine("DAISY: Packet info - msg_type=0x%02X, flags=0x%02X, seq=%u, size=%d bytes",
                        msg_type, flags, sequence_number, (int)packet_size);
    }

    // Handle acknowledgment packets
    if (flags & PKT_FLAG_ACK) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Received ACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        }
        // Handle acknowledgment - remove from retry queue if needed
            return;
        }
        
    // Handle negative acknowledgment packets
    if (flags & PKT_FLAG_NACK) {
            if (s_hw) {
            s_hw->PrintLine("DAISY: Received NACK for msg_type=0x%02X, seq=%u", msg_type, sequence_number);
        }
        // Handle negative acknowledgment - retry if needed
        return;
    }

    // Route based on message type using new unified format
    switch (msg_type) {
        case MSG_SYNC:
            ProcessSyncMessage(packet_data, packet_size);
            break;
        case MSG_CONTROL_CHANGE:
            ProcessControlChangeMessage(packet_data, packet_size);
            break;
        case MSG_NOTE_ON:
            ProcessNoteMessage(packet_data, packet_size);
            break;
        case MSG_NOTE_OFF:
            ProcessNoteOffMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_LOAD:
            ProcessSampleLoadMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_CTRL:
            ProcessSampleControlMessage(packet_data, packet_size);
            break;
        case MSG_PREVIEW_REQ:
            ProcessPreviewRequestMessage(packet_data, packet_size);
            break;
        case MSG_DATA_REQUEST:
            ProcessDataRequestMessage(packet_data, packet_size);
            break;
        case MSG_METER_PUSH:
            ProcessMeterPushMessage(packet_data, packet_size);
            break;
        case MSG_WAVE_CHUNK:
            ProcessWaveChunkMessage(packet_data, packet_size);
            break;
        case MSG_HEARTBEAT:
            ProcessHeartbeatMessage(packet_data, packet_size);
            break;
        case MSG_BROWSE_REQ:
            ProcessBrowseRequestMessage(packet_data, packet_size);
            break;
        case MSG_BROWSE_RESP:
            ProcessBrowseResponseMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_PLAY_REQ:
            ProcessSamplePlayRequestMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_STOP_REQ:
            ProcessSampleStopRequestMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_STATUS:
            ProcessSampleStatusMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_PLAY_INDEX_REQ:
            ProcessSamplePlayIndexRequestMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_GET_PATH_REQ:
            ProcessSampleGetPathRequestMessage(packet_data, packet_size);
            break;
        case MSG_SAMPLE_GET_PATH_RESP:
            ProcessSampleGetPathResponseMessage(packet_data, packet_size);
            break;
        case MSG_ACK:
            ProcessAckMessage(packet_data, packet_size);
            break;
        case MSG_ERROR:
            ProcessErrorMessage(packet_data, packet_size);
            break;
        default:
            if (s_hw) {
                s_hw->PrintLine("DAISY: Unknown message type: 0x%02X", msg_type);
            }
            break;
    }
}

// Legacy function kept for compatibility - redirects to new unified function
static void ProcessSpiMessage(const uint8_t* packet_data, size_t packet_size)
{
    // For legacy compatibility, extract the message type from the unified packet format
    uint8_t msg_type = packet_data[1];
    uint8_t flags = packet_data[0] & PKT_FLAG_MASK;
    uint16_t sequence_number = packet_data[2] | (packet_data[3] << 8);

    ProcessSpiMessageByType(msg_type, flags, sequence_number, packet_data, packet_size);
}

// Process browse request message
static void ProcessBrowseRequestMessage(const uint8_t* packet_data, size_t packet_size)
{
    using namespace WaveX::Protocol;

    // Extract browse request payload (skip header)
    size_t payload_size = packet_size - 7; // header(5) + crc(2)
    const uint8_t* payload = packet_data + 5;

    // Parse browse request: path (null-terminated) + start_index (4 bytes) + max_entries (1 byte)
    char path[96] = {0};
    size_t path_len = strlen((const char*)payload);
    if (path_len >= sizeof(path)) {
        path_len = sizeof(path) - 1;
    }
    memcpy(path, payload, path_len);
    path[path_len] = '\0';

    size_t start_index = 0;
    uint8_t max_entries = 20; // Default to 20 entries

    if (payload_size >= path_len + 1 + sizeof(uint32_t) + sizeof(uint8_t)) {
        const uint8_t* data_ptr = payload + path_len + 1;
        start_index = *(const uint32_t*)data_ptr;
        data_ptr += sizeof(uint32_t);
        max_entries = *data_ptr;
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
        int result = Spi_SendPreCreatedPacket(response_buffer, pkt_size);
        if (result) {
            WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Sent browse response: %zu entries", entries_written);
        } else {
            WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to send browse response");
        }
    } else {
        WAVEX_LOG_DAISY_MESSAGE(DAISY_SPI_MESSAGE, "Failed to create browse response packet");
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
        // No outgoing messages, send zeros
        memset(tx_buf, 0, buf_size);
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

// Process received packet (static function)
static bool ProcessReceivedPacket(const uint8_t* rx_buf, size_t transfer_size)
{
    if (!rx_buf || transfer_size == 0) {
        return false;
    }

    // Validate and parse the unified packet
    if (!validate_wave_packet(rx_buf, transfer_size)) {
        if (s_hw) s_hw->PrintLine("DAISY: Invalid packet received");
        return false;
    }

    // Parse packet details
    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[2048]; // Buffer for payload data
    size_t payload_size;
    
    if (!parse_wave_packet(rx_buf, transfer_size, msg_type, payload, payload_size, sequence_number, flags)) {
        if (s_hw) s_hw->PrintLine("DAISY: Failed to parse packet");
        return false;
    }

    // Route message by type
    ProcessSpiMessageByType(msg_type, flags, sequence_number, payload, payload_size);
    return true;
}

// Perform bidirectional poll (static function)
static bool PerformBidirectionalPoll()
{
    // Send outgoing packets if we have any
    if (outgoing_count > 0) {
        uint8_t* outgoing_msg = outgoing_queue[outgoing_head];
        size_t packet_size = get_packet_size(outgoing_msg);
        
        // Send the packet
        daisy::SpiHandle::Result result = Spi_SendPacket(outgoing_msg, packet_size);
        
        if (result == daisy::SpiHandle::Result::OK) {
            // Successfully sent, remove from queue
            outgoing_head = (outgoing_head + 1) % MAX_QUEUED_MESSAGES;
            outgoing_count--;
            s_stats.packets_sent++;
            
            if (s_hw) {
                uint8_t msg_type = outgoing_msg[1];
                s_hw->PrintLine("DAISY: Sent packet type=0x%02X, size=%zu, remaining=%d", 
                               msg_type, packet_size, outgoing_count);
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

} // namespace Comm
} // namespace WaveX

// Stub implementations for message processing functions (outside namespace)
static void ProcessSyncMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSyncMessage called");
}

static void ProcessControlChangeMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessControlChangeMessage called");
}

static void ProcessNoteMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessNoteMessage called");
}

static void ProcessNoteOffMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessNoteOffMessage called");
}

static void ProcessSampleLoadMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleLoadMessage called");
}

static void ProcessSampleControlMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleControlMessage called");
}

static void ProcessPreviewRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessPreviewRequestMessage called");
}

static void ProcessDataRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessDataRequestMessage called");
}

static void ProcessMeterPushMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessMeterPushMessage called");
}

static void ProcessWaveChunkMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessWaveChunkMessage called");
}

static void ProcessHeartbeatMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessHeartbeatMessage called");
}

static void ProcessBrowseResponseMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessBrowseResponseMessage called");
}

static void ProcessBrowseRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessBrowseRequestMessage called");
    // Parse the payload to extract browse request parameters
    if (packet_size > 0) {
        const char* path = reinterpret_cast<const char*>(packet_data);
        size_t path_len = strlen(path);
        uint8_t max_entries = 10; // Default value
        WaveX::Comm::ProcessBrowseRequest(path, path_len, max_entries);
    }
}

static void ProcessSamplePlayRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSamplePlayRequestMessage called");
    // Parse the payload to extract file path
    if (packet_size > 0) {
        const char* file_path = reinterpret_cast<const char*>(packet_data);
        WaveX::Comm::ProcessSamplePlayRequest(file_path);
    }
}

static void ProcessSampleStopRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleStopRequestMessage called");
}

static void ProcessSampleStatusMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleStatusMessage called");
}

static void ProcessSamplePlayIndexRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSamplePlayIndexRequestMessage called");
}

static void ProcessSampleGetPathRequestMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleGetPathRequestMessage called");
}

static void ProcessSampleGetPathResponseMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessSampleGetPathResponseMessage called");
}

static void ProcessAckMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessAckMessage called");
}

static void ProcessErrorMessage(const uint8_t* packet_data, size_t packet_size) {
    if (s_hw) s_hw->PrintLine("DAISY: ProcessErrorMessage called");
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

        // Process the message directly using new unified format
        ProcessSpiMessageByType(msg_type, flags, sequence_number, packet_data, packet_size);
        
        // Dequeue
        queue_head = (queue_head + 1) % MAX_QUEUED_MESSAGES;
        queue_count--;
        
        WAVEX_LOG_DAISY_INBOUND(DAISY_INBOUND_SPI, "Message processed and dequeued, remaining queue_count=%d", queue_count);
    }
}

#endif // WAVEX_SPI_LINK_ENABLED

