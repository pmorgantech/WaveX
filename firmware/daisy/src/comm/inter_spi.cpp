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

// Use shared packet structures
#define CMD_PKT_SIZE WaveX::Protocol::SPI_CMD_PKT_SIZE
#define DATA_PKT_SIZE WaveX::Protocol::SPI_DATA_PKT_SIZE

typedef WaveX::Protocol::SpiCommandPacket cmd_pkt_t;
typedef WaveX::Protocol::SpiDataPacket data_pkt_t;

// Legacy compatibility - use command packet size as default
#define CTRL_PKT_SIZE CMD_PKT_SIZE

// Use shared CRC function
#define crc16_ccitt WaveX::Protocol::ProtocolHandler::CalculateSpiCrc

// Use shared packet functions
#define is_command_packet WaveX::Protocol::ProtocolHandler::IsCommandPacket
#define is_data_packet WaveX::Protocol::ProtocolHandler::IsDataPacket
#define validate_packet WaveX::Protocol::ProtocolHandler::ValidateSpiPacket
#define get_packet_size WaveX::Protocol::ProtocolHandler::GetSpiPacketSize

// ============================================================================
// Module-level static variables
// ============================================================================

static daisy::DaisySeed* s_hw = NULL;
static daisy::SpiHandle* g_spi_handle = NULL;
static daisy::GPIO cs_pin;
static daisy::GPIO attn_pin;
static WaveX::Comm::spi_link_stats_t s_stats = {};

// Message queue for received messages (unified for both packet types)
#define MAX_QUEUED_MESSAGES 8
static uint8_t message_queue[MAX_QUEUED_MESSAGES][DATA_PKT_SIZE] __attribute__((aligned(4)));
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

// Outgoing message queue (Daisy → ESP32)
static uint8_t outgoing_queue[MAX_QUEUED_MESSAGES][DATA_PKT_SIZE] __attribute__((aligned(4)));
static int outgoing_head = 0;
static int outgoing_tail = 0;
static int outgoing_count = 0;

// Forward declarations will be inside namespace

#if WAVEX_SPI_DMA_ENABLED
static volatile bool s_tx_inflight = false;
// DMA buffers in non-cacheable SRAM for proper coherency
static uint8_t s_tx_dma_buf[DATA_PKT_SIZE] __attribute__((section(".dma_buffer"))) __attribute__((aligned(32)));
static uint8_t s_rx_dma_buf[DATA_PKT_SIZE] __attribute__((section(".dma_buffer"))) __attribute__((aligned(32)));

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
 * @brief Send a packet to ESP32 (one-way). Uses DMA when enabled.
 * Supports both command (32-byte) and data (256-byte) packets.
 */
static daisy::SpiHandle::Result Spi_SendPacket(const uint8_t* tx_buf, size_t packet_size)
{
    if(!g_spi_handle) {
        if (s_hw) s_hw->PrintLine("Spi_SendPacket ERROR: g_spi_handle is NULL!");
        return daisy::SpiHandle::Result::ERR;
    }

    // Validate packet size
    if (packet_size != CMD_PKT_SIZE && packet_size != DATA_PKT_SIZE) {
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
    SCB_CleanDCache_by_Addr((uint32_t*)s_tx_dma_buf, packet_size);

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

// Forward declarations for outgoing message queue functions
static bool QueueOutgoingMessage(const uint8_t* packet_data, size_t packet_size);
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
static void ProcessBrowseRequest(const char* path);
static void ProcessSamplePlayRequest(const char* file_path);
static void ProcessSampleStopRequest();

static void ProcessSpiMessage(const uint8_t* packet_data, size_t packet_size)
{
    using namespace WaveX::Protocol;

    // Debug: Log received packet bytes
    if (s_hw) {
        s_hw->PrintLine("DAISY: Received packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                        packet_data[0], packet_data[1], packet_data[2], packet_data[3],
                        packet_data[4], packet_data[5], packet_data[6], packet_data[7]);
    }
    
    // Validate packet first
    if (!validate_packet(packet_data, packet_size)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid packet received, dropping");
        }
        return;
    }

    uint8_t packet_type = packet_data[0]; // Packet type (0x01=CMD, 0x02=DATA)
    uint8_t len = packet_data[3];
    const uint8_t* payload = &packet_data[6]; // Payload is now at offset 6 (after header + CRC)
    
    // Determine message type based on packet format
    uint8_t type;
    if (packet_type == 0x01 && len > 0) {
        // New format: message type is in first byte of payload
        type = payload[0];
    } else if (packet_type == 0x01 && len == 0) {
        // No-data response: treat as MSG_ACK
        type = MSG_ACK;
    } else {
        // Fallback: use packet type as message type
        type = packet_type;
    }

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
            if (packet_type == 0x01 && len > 1) {
                // New format: skip the message type byte (first byte of payload)
                memcpy(path, &payload[1], len - 1);
                path[len - 1] = '\0';
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
            if (len > 0) {
                char file_path[96] = {0};
                if (packet_type == 0x01 && len > 1) {
                    // New format: skip the message type byte (first byte of payload)
                    size_t copy_len = (len - 1) < (sizeof(file_path) - 1) ? (len - 1) : (sizeof(file_path) - 1);
                    memcpy(file_path, &payload[1], copy_len);
                    file_path[copy_len] = '\0';
                }
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
            if (s_hw) {
                s_hw->PrintLine("DAISY: Unknown message type: 0x%02X", type);
            }
            break;
    }
}

void ProcessEsp32Message(uint8_t type, uint8_t flags, const uint8_t* payload, uint8_t len)
{
    // Legacy compatibility - create packet structure
    uint8_t packet_data[CMD_PKT_SIZE] = {0};
    packet_data[0] = 0x01; // Command packet type
    packet_data[1] = flags;
    packet_data[2] = 0; // Sequence number (not used in legacy mode)
    packet_data[3] = len;
    
    if (payload && len > 0) {
        memcpy(&packet_data[4], payload, len < 20 ? len : 20);
    }
    
    // Calculate CRC
    uint16_t crc = crc16_ccitt(packet_data, 4 + len);
    packet_data[4 + len] = crc & 0xFF;
    packet_data[4 + len + 1] = (crc >> 8) & 0xFF;
    
    // Process the packet
    ProcessSpiMessage(packet_data, CMD_PKT_SIZE);

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

    // Create command packet
    cmd_pkt_t cmd_pkt = {0};
    cmd_pkt.type = 0x01; // Command packet type
    cmd_pkt.flags = 0;   // No special flags
    cmd_pkt.seq = 0;     // Sequence number (not used for now)
    
    // First byte of payload should be the message type
    cmd_pkt.payload[0] = (uint8_t)(type & 0xFF);
    cmd_pkt.len = 1; // Start with message type
    
    // Add actual payload data
    if (payload && len > 0) {
        size_t remaining_space = 20 - 1; // 20 total - 1 for message type
        size_t copy_len = (len > remaining_space) ? remaining_space : len;
        memcpy(&cmd_pkt.payload[1], payload, copy_len);
        cmd_pkt.len += copy_len;
    }
    
    // Calculate CRC
    cmd_pkt.crc = crc16_ccitt((uint8_t*)&cmd_pkt, 4 + cmd_pkt.len);

    // Queue the message for bidirectional transmission
    bool queued = QueueOutgoingMessage((uint8_t*)&cmd_pkt, CMD_PKT_SIZE);
    
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

// Send large packet (data packet format) - for bulk data like browse responses
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
    
    if (packet_size > 240) { // Max data payload size
        if (s_hw) s_hw->PrintLine("Spi_SendLargePacket ERROR: Packet too large: %d bytes", (int)packet_size);
        return 0;
    }
    
    // Create data packet
    data_pkt_t data_pkt = {0};
    data_pkt.type = 0x02; // Data packet type
    data_pkt.flags = 0;   // No special flags
    data_pkt.seq = 0;     // Sequence number (not used for now)
    data_pkt.len = packet_size;
    
    memcpy(data_pkt.payload, packet_data, packet_size);
    
    // Calculate CRC
    data_pkt.crc = crc16_ccitt((uint8_t*)&data_pkt, 4 + data_pkt.len);
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Sending large packet - size=%d bytes", (int)packet_size);
        s_hw->PrintLine("DAISY: Data packet header: %02X %02X %02X %02X", 
                       data_pkt.type, data_pkt.flags, data_pkt.seq, data_pkt.len);
    }
    
    // Send using the new packet function
    daisy::SpiHandle::Result result = Spi_SendPacket((uint8_t*)&data_pkt, DATA_PKT_SIZE);
    
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
    uint8_t* outgoing_msg = outgoing_queue[outgoing_head];
    
    // Determine packet size based on type
    size_t packet_size = get_packet_size(outgoing_msg);
    size_t copy_size = (buf_size < packet_size) ? buf_size : packet_size;
    memcpy(tx_buf, outgoing_msg, copy_size);
    
    // Remove from queue
    outgoing_head = (outgoing_head + 1) % MAX_QUEUED_MESSAGES;
    outgoing_count--;
    
    if (s_hw) {
        // s_hw->PrintLine("DAISY: Prepared outgoing message type=0x%02X, len=%d, remaining=%d", 
        //                outgoing_msg[0], outgoing_msg[3], outgoing_count);
    }
}

// Add message to outgoing queue
static bool QueueOutgoingMessage(const uint8_t* packet_data, size_t packet_size)
{
    if (outgoing_count >= MAX_QUEUED_MESSAGES) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Outgoing queue full, dropping message type=0x%02X", packet_data[0]);
        }
        return false;
    }
    
    // For meter data, check if we already have meter data queued to prevent spam
    if (packet_data[0] == 0x01 && packet_data[4] == WaveX::Protocol::MSG_METER_PUSH) {
        for (int i = 0; i < outgoing_count; i++) {
            int idx = (outgoing_head + i) % MAX_QUEUED_MESSAGES;
            if (outgoing_queue[idx][0] == 0x01 && outgoing_queue[idx][4] == WaveX::Protocol::MSG_METER_PUSH) {
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
    
    uint8_t* msg = outgoing_queue[outgoing_tail];
    memcpy(msg, packet_data, packet_size);
    
    outgoing_tail = (outgoing_tail + 1) % MAX_QUEUED_MESSAGES;
    outgoing_count++;
    
    // Reduce logging for meter data to avoid spam
    if (s_hw && !(packet_data[0] == 0x01 && packet_data[4] == WaveX::Protocol::MSG_METER_PUSH)) {
        // s_hw->PrintLine("DAISY: Queued outgoing message type=0x%02X, len=%d, count=%d", 
        //                packet_data[0], packet_data[3], outgoing_count);
    } else if (s_hw && packet_data[0] == 0x01 && packet_data[4] == WaveX::Protocol::MSG_METER_PUSH && outgoing_count % 10 == 1) {
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
    uint8_t rx_buf[DATA_PKT_SIZE] = {0};
    uint8_t tx_buf[DATA_PKT_SIZE] = {0};
    size_t transfer_size = CMD_PKT_SIZE; // Default to command packet size
    
    // Prepare TX buffer based on the situation
    if (esp32_urgent) {
        // ESP32 has urgent data - send empty packet to trigger ESP32 response
        // ESP32 will respond with its queued data during the transaction
        memset(tx_buf, 0, DATA_PKT_SIZE);
        transfer_size = CMD_PKT_SIZE; // Start with command packet size
        if (s_hw) {
            s_hw->PrintLine("DAISY: Requesting urgent data from ESP32 (no poll needed)");
        }
    } else if (we_have_data) {
        // We have data to send - prepare our outgoing data
        PrepareTxBuffer(tx_buf, DATA_PKT_SIZE);
        // Determine actual packet size from the prepared buffer
        transfer_size = get_packet_size(tx_buf);
        if (transfer_size == 0) transfer_size = CMD_PKT_SIZE; // Fallback
        // if (s_hw && poll_count % 10 == 0) { // Reduce logging
        //     s_hw->PrintLine("DAISY: Sending queued data to ESP32, type=0x%02X", tx_buf[0]);
        // }
    } else {
        // No urgent data and no outgoing data - skip communication
        return false;
    }
    
    // Set up SPI transaction
    cs_pin.Write(false); // Assert CS
    System::DelayUs(10); // Small delay for setup
    
    // Send poll request and receive response
    daisy::SpiHandle::Result result = g_spi_handle->BlockingTransmitAndReceive(
        tx_buf, rx_buf, transfer_size, 100); // Use appropriate transfer size
    
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
    if (!(rx_buf[0] == 0x01 && rx_buf[1] == 0x00 && rx_buf[2] == 0x00 && rx_buf[3] == 0x01 && rx_buf[4] == 0xFF)) {
        for (int i = 0; i < DATA_PKT_SIZE; i++) {
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
    
    // Determine packet size and validate
    size_t packet_size = get_packet_size(rx_buf);
    if (s_hw) {
        s_hw->PrintLine("DAISY: get_packet_size returned %d for type 0x%02X", (int)packet_size, rx_buf[0]);
    }
    if (packet_size == 0) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Unknown packet type: 0x%02X", rx_buf[0]);
        }
        return false;
    }
    
    // Check for empty or invalid data packets (all 0xFF or all 0x00)
    bool is_empty_data = true;
    for (int i = 0; i < 8; i++) {
        if (rx_buf[i] != 0xFF && rx_buf[i] != 0x00) {
            is_empty_data = false;
            break;
        }
    }
    
    if (is_empty_data) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Received empty/invalid data packet, ignoring");
        }
        return false;
    }
    
    // Validate the packet
    if (s_hw) {
        s_hw->PrintLine("DAISY: Validating packet of size %d", (int)packet_size);
        s_hw->PrintLine("DAISY: Packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                        rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], 
                        rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
    }
    
    // Check packet type and length
    uint8_t type = rx_buf[0];
    uint8_t len = rx_buf[3];
    if (s_hw) {
        s_hw->PrintLine("DAISY: Packet type=0x%02X, len=%d", type, len);
    }
    
    // Check CRC
    uint16_t received_crc = rx_buf[4] | (rx_buf[5] << 8);
    
    // Calculate CRC over header + payload (excluding CRC field)
    uint8_t crc_data[4 + 20]; // Header (4) + max payload (20)
    crc_data[0] = rx_buf[0]; // type
    crc_data[1] = rx_buf[1]; // flags
    crc_data[2] = rx_buf[2]; // seq
    crc_data[3] = rx_buf[3]; // len
    memcpy(&crc_data[4], &rx_buf[6], len); // payload (starts at offset 6)
    
    uint16_t calculated_crc = crc16_ccitt(crc_data, 4 + len);
    if (s_hw) {
        s_hw->PrintLine("DAISY: CRC calculation: %d bytes - %02X %02X %02X %02X %02X", 
                        4 + len, crc_data[0], crc_data[1], crc_data[2], crc_data[3], crc_data[4]);
        s_hw->PrintLine("DAISY: Received CRC=0x%04X, Calculated CRC=0x%04X", received_crc, calculated_crc);
    }
    
    if (!validate_packet(rx_buf, packet_size)) {
        if (s_hw) {
            s_hw->PrintLine("DAISY: Invalid packet received, dropping");
        }
        return false;
    }
    
    if (s_hw) {
        s_hw->PrintLine("DAISY: Received packet type=0x%02X, len=%d, size=%d", 
                        rx_buf[0], rx_buf[3], (int)packet_size);
        s_hw->PrintLine("DAISY: Full packet data: %02X %02X %02X %02X %02X %02X %02X %02X", 
                        rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
    }
    
    // Queue the message for processing
    if (queue_count < MAX_QUEUED_MESSAGES) {
        memcpy(message_queue[queue_tail], rx_buf, packet_size);
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
        uint8_t* packet_data = message_queue[queue_head];
        if (s_hw) {
            s_hw->PrintLine("DAISY: Spi_Recv - dequeuing message type=0x%02X, len=%d, queue_count=%d", 
                           packet_data[0], packet_data[3], queue_count);
        }
        
        // Convert to pkt_t format for legacy compatibility
        memset(&converted_packet, 0, sizeof(converted_packet));
        
        uint8_t packet_type = packet_data[0];
        uint8_t len = packet_data[3];
        const uint8_t* payload = &packet_data[6]; // Payload is now at offset 6 (after header + CRC)
        
        if (packet_type == 0x01 && len > 0) {
            // Command packet - message type is first byte of payload
            converted_packet.h.type = payload[0];
            converted_packet.h.len = len - 1; // Subtract message type byte
            
            // Copy payload (skip message type byte)
            if (converted_packet.h.len > 0 && converted_packet.h.len <= sizeof(converted_packet.payload)) {
                memcpy(converted_packet.payload, &payload[1], converted_packet.h.len);
            }
        } else {
            // Data packet or invalid - use packet type as message type
            converted_packet.h.type = packet_type;
            converted_packet.h.len = len;
            
            if (len > 0 && len <= sizeof(converted_packet.payload)) {
                memcpy(converted_packet.payload, payload, len);
            }
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
