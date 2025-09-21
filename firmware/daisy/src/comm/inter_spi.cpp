#include "inter_spi.h"

#if WAVEX_SPI_LINK_ENABLED

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
#include "../storage/fs_browse.h" // For file browsing
#include "../audio/audio_engine.h" // For sample audition

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
static daisy::GPIO attn_pin;
static WaveX::Comm::spi_link_stats_t s_stats = {};

// Message queue for received messages
#define MAX_QUEUED_MESSAGES 8
static ctrl_pkt_t message_queue[MAX_QUEUED_MESSAGES];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

// Outgoing message queue (Daisy → ESP32)
static ctrl_pkt_t outgoing_queue[MAX_QUEUED_MESSAGES];
static int outgoing_head = 0;
static int outgoing_tail = 0;
static int outgoing_count = 0;

// Forward declarations will be inside namespace

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

// ============================================================================
// SPI Transaction with Response Reception
// ============================================================================

/**
 * @brief Send a single 64-byte frame to ESP32 (one-way). Uses DMA when enabled.
 */
static daisy::SpiHandle::Result Spi_SendRaw64(const uint8_t* tx_buf)
{
    // if (s_hw) s_hw->PrintLine("Spi_SendRaw64 called");

    if(!g_spi_handle) {
        // if (s_hw) s_hw->PrintLine("Spi_SendRaw64 ERROR: g_spi_handle is NULL!");
        return daisy::SpiHandle::Result::ERR;
    }

    // if (s_hw) s _hw->PrintLine("Spi_SendRaw64: g_spi_handle is valid");

#if WAVEX_SPI_DMA_ENABLED
    // if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Using DMA path");

    if(s_tx_inflight) {
        // if (s_hw) s_hw->PrintLine("Spi_SendRaw64: DMA transaction already in flight, returning ERR");
        return daisy::SpiHandle::Result::ERR;
    }

    // if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Copying payload to DMA buffer");
    // Copy payload into persistent DMA buffer
    memcpy(s_tx_dma_buf, tx_buf, CTRL_PKT_SIZE);
    // Ensure cache coherency for DMA
    dsy_dma_clear_cache_for_buffer(s_tx_dma_buf, CTRL_PKT_SIZE);

    s_tx_inflight = true;
    // if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Starting DMA transfer");

    // Start DMA transfer (one-way); CS low/high handled in callbacks
    daisy::SpiHandle::Result dma_result = g_spi_handle->DmaTransmit(
        s_tx_dma_buf,
        CTRL_PKT_SIZE,
        spi_dma_start_cb,
        spi_dma_end_cb,
        NULL);

    // if (s_hw) s_hw->PrintLine("Spi_SendRaw64: DMA transfer initiated, result: %d", (int)dma_result);
    return dma_result;
#else
    if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Using blocking path");

    // Fallback: blocking transmit
    if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Setting CS low");
    cs_pin.Write(false);

    if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Calling BlockingTransmit");
    daisy::SpiHandle::Result res = g_spi_handle->BlockingTransmit((uint8_t*)tx_buf, CTRL_PKT_SIZE, 100);

    if (s_hw) s_hw->PrintLine("Spi_SendRaw64: BlockingTransmit returned: %d", (int)res);

    if (s_hw) s_hw->PrintLine("Spi_SendRaw64: Setting CS high");
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

// Forward declarations for outgoing message queue functions
static bool QueueOutgoingMessage(uint8_t type, const uint8_t* payload, uint8_t len);
static bool Spi_HasOutgoingData();
static void PrepareTxBuffer(uint8_t* tx_buf, size_t buf_size);

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
static void ProcessBrowseRequest(const char* path);
static void ProcessSamplePlayRequest(const char* file_path);
static void ProcessSampleStopRequest();

static void ProcessSpiMessage(uint8_t type, const uint8_t* payload, uint8_t len)
{
    using namespace WaveX::Protocol;

    // Process incoming SPI messages from ESP32
    switch (type) {
        case MSG_SYNC: {
            // Synchronization message - acknowledge receipt
            if (s_hw) {
                s_hw->PrintLine("DAISY: Received MSG_SYNC (0x00), acknowledging");
            }
            // For now, just acknowledge the sync - could add sync-specific logic later
            break;
        }
        
        case MSG_BROWSE_REQ: {
            // ESP32 requesting directory listing
            if (s_hw) {
                s_hw->PrintLine("DAISY: Processing MSG_BROWSE_REQ, payload len=%d", len);
            }
            
            char path[96] = {0};
            if (len > 0 && len < sizeof(path)) {
                memcpy(path, payload, len);
                path[len] = '\0';
            } else {
                // Default to root if no path provided
                strcpy(path, "/");
            }

            if (s_hw) {
                s_hw->PrintLine("DAISY: Browse request path: '%s'", path);
            }

            // Process browse request
            ProcessBrowseRequest(path);
            break;
        }

        case MSG_SAMPLE_PLAY_REQ: {
            // ESP32 requesting sample audition
            if (len >= 1) {
                char file_path[96] = {0};
                memcpy(file_path, payload, len < sizeof(file_path) ? len : sizeof(file_path) - 1);
                ProcessSamplePlayRequest(file_path);
            }
            break;
        }

        case MSG_SAMPLE_STOP_REQ: {
            // ESP32 requesting to stop sample audition
            ProcessSampleStopRequest();
            break;
        }

        default:
            // Unknown message type - ignore
            break;
    }
}

void ProcessEsp32Message(uint8_t type, uint8_t flags, const uint8_t* payload, uint8_t len)
{
    // Process messages received from ESP32 by calling the message processor
    ProcessSpiMessage(type, payload, len);

    if (s_hw) {
        s_hw->PrintLine("Processed ESP32 message type 0x%02X, len=%d", type, len);
    }
}

int Spi_Send(uint16_t type, const void* payload, uint16_t len)
{
    if (!g_spi_handle) {
        if (s_hw) s_hw->PrintLine("Spi_Send ERROR: g_spi_handle is NULL!");
        return 0;
    }

    // Queue the message for bidirectional transmission
    bool queued = QueueOutgoingMessage((uint8_t)(type & 0xFF), (const uint8_t*)payload, (uint8_t)len);
    
    if (queued) {
        // Reduce logging for meter data
        if (s_hw && type != WaveX::Protocol::MSG_METER_PUSH) {
            // s_hw->PrintLine("DAISY: Queued message type=0x%02X for next SPI transaction", (uint8_t)(type & 0xFF));
        }
        return 1;
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Failed to queue message type=0x%02X", (uint8_t)(type & 0xFF));
        }
        return 0;
    }
}

// Send large packet (pkt_t format) - for bulk data like browse responses
int Spi_SendLargePacket(const uint8_t* packet_data, size_t packet_size)
{
    if (!g_spi_handle) {
        if (s_hw) s_hw->PrintLine("Spi_SendLargePacket ERROR: g_spi_handle is NULL!");
        return 0;
    }
    
    if (!packet_data || packet_size == 0) {
        if (s_hw) s_hw->PrintLine("Spi_SendLargePacket ERROR: Invalid packet data");
        return 0;
    }
    
    if (packet_size > 246) { // Max pkt_t size
        if (s_hw) s_hw->PrintLine("Spi_SendLargePacket ERROR: Packet too large: %d bytes", (int)packet_size);
        return 0;
    }
    
    // Create aligned pkt_t buffer
    static uint8_t large_tx_buf[246] __attribute__((aligned(4)));
    memset(large_tx_buf, 0, sizeof(large_tx_buf));
    memcpy(large_tx_buf, packet_data, packet_size);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Sending large packet - size=%d bytes", (int)packet_size);
        s_hw->PrintLine("DAISY: Large packet header: %02X %02X %02X %02X", 
                       large_tx_buf[0], large_tx_buf[1], large_tx_buf[2], large_tx_buf[3]);
    }
    
    // Send using SPI (CS management by caller)
    cs_pin.Write(false); // Assert CS
    System::DelayUs(10); // Small delay for setup
    
    daisy::SpiHandle::Result result = g_spi_handle->BlockingTransmit(large_tx_buf, packet_size, 1000);
    
    cs_pin.Write(true); // Deassert CS
    
    if (result == daisy::SpiHandle::Result::OK) {
        s_stats.packets_sent++;
        if (s_hw) s_hw->PrintLine("DAISY: Large packet sent successfully");
        return 1;
    } else {
        if (s_hw) s_hw->PrintLine("DAISY: Large packet send FAILED: %d", (int)result);
        return 0;
    }
}

int Spi_Poll_For_Response(void)
{
    // Regular SPI slave currently does not push responses; not used.
    return 0;
}


// Stubs for the rest of the API
// ============================================================================
// Forward Declarations
// ============================================================================

// ============================================================================
// Message Processing Functions
// ============================================================================


// ============================================================================
// Message Processing Implementations
// ============================================================================

static void ProcessBrowseRequest(const char* path)
{
    using namespace WaveX::Storage;
    using namespace WaveX::Protocol;

    // Log browse request receipt
    if (s_hw) {
        s_hw->PrintLine("DAISY: Received browse request for path: '%s'", path);
    }

    // Buffer for response packet  
    uint8_t response_buffer[700]; // Large enough for header + 10 file entries (~53 bytes each)

    // Get directory listing
    FileEntry entries[4]; // Max 4 entries per response to fit in 255-byte packet limit
    size_t total_count = 0;
    size_t start_index = 0; // TODO: Support pagination in future

    if (s_hw) {
        s_hw->PrintLine("DAISY: Calling ListDir for path: '%s'", path);
    }

    bool success = ListDir(path, entries, sizeof(entries)/sizeof(entries[0]), total_count, start_index);

    if (s_hw) {
        s_hw->PrintLine("DAISY: ListDir result: success=%d, total_count=%d", success, (int)total_count);
    }

    if (!success) {
        // Send error response
        if (s_hw) {
            s_hw->PrintLine("DAISY: Directory read failed, sending error response");
        }
        ErrorMessage err = {0x01, "Directory read failed"};
        size_t pkt_size = ProtocolHandler::CreateErrorPacket(response_buffer, sizeof(response_buffer), err);
        Spi_Send(MSG_ERROR, response_buffer + sizeof(PacketHeader), pkt_size - sizeof(PacketHeader));
        return;
    }

    // Convert FileEntry to FileEntryWire format
    FileEntryWire wire_entries[4];
    uint8_t num_entries = 0;

    if (s_hw) {
        s_hw->PrintLine("DAISY: Converting %d file entries to wire format", (int)total_count);
    }

    for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]) && num_entries < 4; i++) {
        if (entries[i].name[0] == '\0') break; // End of valid entries

        wire_entries[num_entries].is_dir = entries[i].is_dir;
        wire_entries[num_entries].size_bytes = entries[i].size_bytes;
        std::strncpy(wire_entries[num_entries].name, entries[i].name, sizeof(wire_entries[num_entries].name) - 1);
        wire_entries[num_entries].name[sizeof(wire_entries[num_entries].name) - 1] = '\0';
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Entry %d: '%s' (%s, %d bytes)", 
                           num_entries, 
                           wire_entries[num_entries].name,
                           wire_entries[num_entries].is_dir ? "DIR" : "FILE",
                           (int)wire_entries[num_entries].size_bytes);
        }
        
        num_entries++;
    }

    if (s_hw) {
        s_hw->PrintLine("DAISY: Sending browse response with %d entries (total_count=%d)", num_entries, (int)total_count);
    }

    // Send browse response using large packet format (pkt_t)
    if (s_hw) {
        s_hw->PrintLine("DAISY: About to call CreateBrowseRespPacket - buffer_size=%d, total_count=%d, wire_entries=%p, num_entries=%d", 
                       (int)sizeof(response_buffer), (int)total_count, wire_entries, num_entries);
        
        // Debug: Show first entry details
        if (num_entries > 0) {
            s_hw->PrintLine("DAISY: First entry - is_dir=%d, size=%d, name='%s'", 
                           wire_entries[0].is_dir, (int)wire_entries[0].size_bytes, wire_entries[0].name);
        }
    }
    // Debug: Manually check packet calculation before calling function
    if (s_hw) {
        size_t expected_payload = 5 + (size_t)num_entries * 53; // BrowseRespHeader + FileEntryWire entries
        s_hw->PrintLine("DAISY: Expected payload size = 5 + %d * 53 = %d bytes (max 255)", num_entries, (int)expected_payload);
        s_hw->PrintLine("DAISY: wire_entries pointer check - first entry name: '%.10s'", wire_entries[0].name);
    }
    
    // TEMPORARY: Manual packet creation to debug the issue
    if (s_hw) {
        s_hw->PrintLine("DAISY: TEMP - Manually creating packet to debug");
        s_hw->PrintLine("DAISY: TEMP - BrowseRespHeader size: %d", (int)sizeof(WaveX::Protocol::BrowseRespHeader));
        s_hw->PrintLine("DAISY: TEMP - FileEntryWire size: %d", (int)sizeof(WaveX::Protocol::FileEntryWire));
    }
    
    // Use original CreateBrowseRespPacket with corrected approach
    // ESP32 expects PacketHeader format, not pkt_t format
    // Maximum payload size with PacketHeader: 252 bytes (256 - 4 byte header)
    // This allows: (252 - 5) / 53 = 4.6 → 4 entries max
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Using PacketHeader format for ESP32 compatibility");
        s_hw->PrintLine("DAISY: Expected payload = 5 + %d * 53 = %d bytes (max ~250)", 
                       num_entries, (int)(5 + num_entries * 53));
    }
    
    // Use CreateBrowseRespPacket which creates proper Packet structure
    size_t pkt_size = ProtocolHandler::CreateBrowseRespPacket(response_buffer, sizeof(response_buffer),
                                           total_count, wire_entries, num_entries);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: CreateBrowseRespPacket returned %d bytes", (int)pkt_size);
    }
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Created browse response packet, size=%d bytes", (int)pkt_size);
    }
    
    // Send as large packet - try with longer delay to ensure ESP32 is ready
    if (s_hw) {
        s_hw->PrintLine("DAISY: Waiting for ESP32 to be ready for large packet...");
    }
    
    // Wait longer to ensure ESP32 has finished processing the browse request
    System::Delay(100); // 100ms delay
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Sending large packet to ESP32 now...");
    }
    
    int send_result = Spi_SendLargePacket(response_buffer, pkt_size);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Large packet send result: %d", send_result);
    }
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Browse response sent successfully");
    }
}

static void ProcessSamplePlayRequest(const char* file_path)
{
    using namespace WaveX::Protocol;

    // Start sample audition
    bool success = WaveX::AudioEngine::AuditionSample(file_path);

    // Send status response
    SampleStatusMessage status;
    if (success) {
        status.state = 1; // Playing
        status.sample_rate = 44100; // TODO: Get actual sample rate
        status.channels = 2; // TODO: Get actual channel count
        status.frames_played = 0;
    } else {
        status.state = 0; // Stopped (failed to start)
        status.sample_rate = 0;
        status.channels = 0;
        status.frames_played = 0;
    }

    uint8_t response_buffer[64];
    size_t pkt_size = ProtocolHandler::CreateSampleStatusPacket(response_buffer, sizeof(response_buffer), status);
    Spi_Send(MSG_SAMPLE_STATUS, response_buffer + sizeof(PacketHeader), pkt_size - sizeof(PacketHeader));
}

static void ProcessSampleStopRequest()
{
    using namespace WaveX::Protocol;

    // Stop sample audition
    WaveX::AudioEngine::StopAudition();

    // Send status response
    SampleStatusMessage status;
    status.state = 0; // Stopped
    status.sample_rate = 0;
    status.channels = 0;
    status.frames_played = 0;

    uint8_t response_buffer[64];
    size_t pkt_size = ProtocolHandler::CreateSampleStatusPacket(response_buffer, sizeof(response_buffer), status);
    Spi_Send(MSG_SAMPLE_STATUS, response_buffer + sizeof(PacketHeader), pkt_size - sizeof(PacketHeader));
}


// ============================================================================
// Outgoing Message Queue Management
// ============================================================================

// Check if we have outgoing data to send
static bool Spi_HasOutgoingData()
{
    return outgoing_count > 0;
}

// Prepare TX buffer with next outgoing message
static void PrepareTxBuffer(uint8_t* tx_buf, size_t buf_size)
{
    if (outgoing_count == 0 || !tx_buf) {
        return;
    }
    
    // Get next outgoing message
    ctrl_pkt_t* outgoing_msg = &outgoing_queue[outgoing_head];
    
    // Copy to TX buffer
    size_t copy_size = (buf_size < CTRL_PKT_SIZE) ? buf_size : CTRL_PKT_SIZE;
    memcpy(tx_buf, outgoing_msg, copy_size);
    
    // Remove from queue
    outgoing_head = (outgoing_head + 1) % MAX_QUEUED_MESSAGES;
    outgoing_count--;
    
    if (s_hw) {
        // s_hw->PrintLine("DAISY: Prepared outgoing message type=0x%02X, len=%d, remaining=%d", 
        //                outgoing_msg->type, outgoing_msg->len, outgoing_count);
    }
}

// Add message to outgoing queue
static bool QueueOutgoingMessage(uint8_t type, const uint8_t* payload, uint8_t len)
{
    if (outgoing_count >= MAX_QUEUED_MESSAGES) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Outgoing queue full, dropping message type=0x%02X", type);
        }
        return false;
    }
    
    // For meter data, check if we already have meter data queued to prevent spam
    if (type == WaveX::Protocol::MSG_METER_PUSH) {
        for (int i = 0; i < outgoing_count; i++) {
            int idx = (outgoing_head + i) % MAX_QUEUED_MESSAGES;
            if (outgoing_queue[idx].type == WaveX::Protocol::MSG_METER_PUSH) {
                // Already have meter data queued - skip this one
                if (s_hw) {
                    s_hw->PrintLine("DAISY: Skipping meter data - already queued (count=%d)", outgoing_count);
                }
                return false;
            }
        }
        // Debug: Log when we're adding meter data
        if (s_hw && outgoing_count == 0) {
            s_hw->PrintLine("DAISY: Adding meter data to empty queue");
        }
    }
    
    ctrl_pkt_t* msg = &outgoing_queue[outgoing_tail];
    memset(msg, 0, sizeof(*msg));
    
    msg->type = type;
    msg->len = (len > 26) ? 26 : len; // Limit to payload size
    if (payload && len > 0) {
        memcpy(msg->payload, payload, msg->len);
    }
    
    outgoing_tail = (outgoing_tail + 1) % MAX_QUEUED_MESSAGES;
    outgoing_count++;
    
    // Reduce logging for meter data to avoid spam
    if (s_hw && type != WaveX::Protocol::MSG_METER_PUSH) {
        // s_hw->PrintLine("DAISY: Queued outgoing message type=0x%02X, len=%d, count=%d", 
        //                type, len, outgoing_count);
    } else if (s_hw && type == WaveX::Protocol::MSG_METER_PUSH && outgoing_count % 10 == 1) {
        s_hw->PrintLine("DAISY: Queued meter data (every 10th logged), count=%d", outgoing_count);
    }
    
    return true;
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Hybrid communication - immediate response to ESP32 attention + send queued data
static bool PerformBidirectionalPoll()
{
    // Check for urgent ESP32 control signal (active high on D0)
    bool esp32_urgent = attn_pin.Read();  // Active high signal from GPIO31
    
    // Debug: Log attention pin state periodically AND when urgent
    static uint32_t debug_count = 0;
    debug_count++;
    
    if (esp32_urgent && s_hw) {
        // Log every urgent signal immediately
        s_hw->PrintLine("DAISY: URGENT detected! Attention pin HIGH (check #%lu)", (unsigned long)debug_count);
    } else if (debug_count % 5000 == 0 && s_hw) {
        // Log normal state less frequently
        s_hw->PrintLine("DAISY: Attention pin state: LOW (check #%lu)", (unsigned long)debug_count);
    }
    
    // Check if we have outgoing data (meter data, heartbeats)
    bool we_have_data = Spi_HasOutgoingData();
    
    // Communicate if either condition is true
    if (!esp32_urgent && !we_have_data) {
        // No reason to communicate
        return false;
    }
    
    static uint32_t poll_count = 0;
    poll_count++;
    
    if (esp32_urgent) {
        // ESP32 has urgent control data - respond immediately
        if (s_hw) {
            s_hw->PrintLine("DAISY: URGENT poll #%lu - ESP32 attention signal active", (unsigned long)poll_count);
        }
    } else if (we_have_data) {
        // We have data to send (meter, heartbeat, etc.)
        if (s_hw && poll_count % 20 == 0) { // Reduce logging
            s_hw->PrintLine("DAISY: Sending queued data, poll #%lu", (unsigned long)poll_count);
        }
    }
    
    // Prepare bidirectional transaction
    uint8_t rx_buf[CTRL_PKT_SIZE] = {0};
    uint8_t tx_buf[CTRL_PKT_SIZE] = {0};
    
    // Always prepare TX buffer - either with our data or a poll request
    if (we_have_data) {
        PrepareTxBuffer(tx_buf, CTRL_PKT_SIZE);
        // if (s_hw && poll_count % 10 == 0) { // Reduce logging
        //     s_hw->PrintLine("DAISY: Sending queued data to ESP32, type=0x%02X", tx_buf[0]);
        // }
    } else {
        // Send poll request - ESP32 might have data for us
        tx_buf[0] = 0xFF; // Poll command  
        if (s_hw && poll_count % 20 == 0) { // Reduce logging
            s_hw->PrintLine("DAISY: Sending poll request to ESP32");
        }
    }
    
    // Set up SPI transaction
    cs_pin.Write(false); // Assert CS
    System::DelayUs(10); // Small delay for setup
    
    // Send poll request and receive response
    daisy::SpiHandle::Result result = g_spi_handle->BlockingTransmitAndReceive(
        tx_buf, rx_buf, CTRL_PKT_SIZE, 100); // 100ms timeout for faster polling
    
    cs_pin.Write(true); // Deassert CS
    
    if (result != daisy::SpiHandle::Result::OK) {
        // Log failed polls occasionally
        static uint32_t failed_polls = 0;
        if (++failed_polls % 1000 == 0) {
            if (s_hw) {
                s_hw->PrintLine("DAISY: SPI poll failed (fail #%lu, result=%d)", (unsigned long)failed_polls, (int)result);
            }
        }
        return false;
    }
    
    // Check if we got a valid response (not all zeros or just the poll echo)
    bool has_meaningful_data = false;
    
    // Skip check if this is just echoing our poll request
    if (!(rx_buf[0] == 0xFF && rx_buf[1] == 0x00 && rx_buf[2] == 0x00 && rx_buf[3] == 0x00)) {
        for (int i = 0; i < CTRL_PKT_SIZE; i++) {
            if (rx_buf[i] != 0) {
                has_meaningful_data = true;
                break;
            }
        }
    }
    
    if (!has_meaningful_data) {
        // Log when we get all zeros occasionally (reduced frequency)
        static uint32_t zero_responses = 0;
        if (++zero_responses % 2000 == 0) {
            if (s_hw) {
                s_hw->PrintLine("DAISY: Received all zeros from ESP32 (zero #%lu)", (unsigned long)zero_responses);
            }
        }
        return false; // No meaningful data available
    }
    
    // Parse the received packet
    ctrl_pkt_t* pkt = (ctrl_pkt_t*)rx_buf;
    
    // Basic validation
    if (pkt->len > 26) {
        s_hw->PrintLine("DAISY: Invalid packet length: %d", pkt->len);
        return false;
    }
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Received packet type=0x%02X, len=%d", pkt->type, pkt->len);
        if (pkt->len > 0) {
            s_hw->PrintLine("DAISY: Packet payload: %.*s", pkt->len, pkt->payload);
        }
        s_hw->PrintLine("DAISY: Full packet data: %02X %02X %02X %02X %02X %02X %02X %02X", 
                        rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
    }
    
    // Queue the message for processing
    if (queue_count < MAX_QUEUED_MESSAGES) {
        message_queue[queue_tail] = *pkt;
        queue_tail = (queue_tail + 1) % MAX_QUEUED_MESSAGES;
        queue_count++;
        if (s_hw) {
            s_hw->PrintLine("DAISY: Queued message, queue_count=%d", queue_count);
        }
        return true;
    } else {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Message queue full, dropping packet");
        }
        return false;
    }
}

// Static buffer to convert ctrl_pkt_t to pkt_t format
static pkt_t converted_packet;

int Spi_Recv(pkt_t **out) 
{ 
    // Perform regular bidirectional polling
    PerformBidirectionalPoll();
    
    // Return queued message if available
    if (queue_count > 0) {
        ctrl_pkt_t* ctrl_msg = &message_queue[queue_head];
        if (s_hw) {
            s_hw->PrintLine("DAISY: Spi_Recv - dequeuing message type=0x%02X, len=%d, queue_count=%d", 
                           ctrl_msg->type, ctrl_msg->len, queue_count);
        }
        
        // Convert ctrl_pkt_t to pkt_t format
        memset(&converted_packet, 0, sizeof(converted_packet));
        converted_packet.h.len = ctrl_msg->len;
        converted_packet.h.type = ctrl_msg->type;
        if (ctrl_msg->len > 0 && ctrl_msg->len <= sizeof(converted_packet.payload)) {
            memcpy(converted_packet.payload, ctrl_msg->payload, ctrl_msg->len);
        }
        
        if (s_hw) {
            s_hw->PrintLine("DAISY: Converted to pkt_t - type=0x%02X, len=%d", 
                           converted_packet.h.type, converted_packet.h.len);
        }
        
        *out = &converted_packet;
        queue_head = (queue_head + 1) % MAX_QUEUED_MESSAGES;
        queue_count--;
        return 1;
    }
    
    *out = NULL;
    return 0;
}
void Spi_ValidateBuffers() {}
void Spi_Recycle(pkt_t *p, int is_rx) {}
void Spi_ForceReset() {}
bool Spi_HasPendingData(void) 
{ 
    // Perform regular bidirectional polling
    PerformBidirectionalPoll();
    
    // Return true if we have queued messages
    return queue_count > 0;
}

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
