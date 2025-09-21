#include "spi_link.h"
#include "spi_protocol/spi_protocol.h"
#include "link_config.h"
#include "../inter_mcu.h"
#include "../../shared/spi_protocol/protocol.h"
#include <string.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif



#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// *** Regular SPI slave driver ***
#include "driver/spi_slave.h"     // requires REQUIRES esp_driver_spi in CMake
#include "driver/gpio.h"
#endif

// -----------------------------
// Packet & CRC
// -----------------------------
static const char *TAG = "spi_link";

#define CTRL_PKT_SIZE WaveX::Protocol::SPI_CMD_PKT_SIZE
#define MAX_PKT_SIZE 256 // Support large packets up to 256 bytes (pkt_t max is 246)

// Use shared packet structure
typedef WaveX::Protocol::SpiCommandPacket ctrl_pkt_t;

// Use shared CRC function
#define wavex_crc16 WaveX::Protocol::ProtocolHandler::CalculateSpiCrc


// Handle large packet format (pkt_t) - for bulk data like browse responses
static void handle_large_packet(const uint8_t* packet_data, size_t packet_len)
{
#ifdef ESP_PLATFORM
    if (packet_len < sizeof(WaveX::Protocol::PacketHeader)) {
        ESP_LOGE(TAG, "Large packet too short: %d bytes", (int)packet_len);
        return;
    }
    
    const WaveX::Protocol::PacketHeader* header = (const WaveX::Protocol::PacketHeader*)packet_data;
    
    ESP_LOGI(TAG, "Large packet: sync=0x%02X, type=0x%02X, len=%u", 
             header->sync, header->type, header->length);
    
    // Validate sync byte
    if (header->sync != WaveX::Protocol::SYNC_BYTE) {
        ESP_LOGE(TAG, "Invalid sync byte: 0x%02X", header->sync);
        return;
    }
    
    // Validate packet length
    if (header->length + sizeof(WaveX::Protocol::PacketHeader) > packet_len) {
        ESP_LOGE(TAG, "Packet length mismatch: header says %u, received %d", 
                 header->length, (int)packet_len);
        return;
    }
    
    // Handle different large packet types
    const uint8_t* payload = packet_data + sizeof(WaveX::Protocol::PacketHeader);
    
    if (header->type == WaveX::Protocol::MSG_BROWSE_RESP) {
        ESP_LOGI(TAG, "Processing large browse response: %u bytes", header->length);
        
        // Forward entire packet (including header) to browse response callback
        extern void inter_mcu_invoke_browse_resp_callback(const uint8_t* data, size_t length);
        inter_mcu_invoke_browse_resp_callback(packet_data, packet_len);
    } else {
        ESP_LOGW(TAG, "Unknown large packet type: 0x%02X", header->type);
    }
    
    // Update packet statistics
    inter_mcu_increment_packet_stat(header->type);
#else
    (void)packet_data;
    (void)packet_len;
#endif
}

// Handle control messages received FROM Daisy (frontend commands to backend)
static void handle_control_message_from_daisy(const ctrl_pkt_t* p)
{
#ifdef ESP_PLATFORM
    // ESP_LOGI(TAG, "Received control message from Daisy: type=0x%02X, len=%u", p->type, p->len);
    
    // Forward control messages to the inter_mcu layer for processing
    extern void inter_mcu_process_daisy_control_message(uint8_t type, const uint8_t* payload, uint8_t len);
    inter_mcu_process_daisy_control_message(p->type, p->payload, p->len);
    
    // Update packet statistics
    inter_mcu_increment_packet_stat(p->type);
#else
    (void)p;
#endif
}

// Handle new format control messages from Daisy (message type in payload)
static void handle_control_message_from_daisy_new_format(uint8_t msg_type, const uint8_t* payload, uint8_t len)
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Received new format control message from Daisy: type=0x%02X, len=%d", msg_type, len);
    
    // Forward control messages to the inter_mcu layer for processing
    extern void inter_mcu_process_daisy_control_message(uint8_t type, const uint8_t* payload, uint8_t len);
    inter_mcu_process_daisy_control_message(msg_type, payload, len);
    
    // Update packet statistics
    inter_mcu_increment_packet_stat(msg_type);
#else
    (void)msg_type;
    (void)payload;
    (void)len;
#endif
}

static void handle_ctrl_packet(const ctrl_pkt_t* p)
{
#ifdef ESP_PLATFORM
    #if WAVEX_MCU_LINK_PACKET_DEBUG
    ESP_LOGI(TAG, "RX pkt: type=0x%02X len=%u seq=%u flags=0x%02X",
              p->type, p->len, p->seq, p->flags);
    #endif

    // Determine packet type for statistics (outside critical section)
    uint8_t stat_packet_type = 0xFF; // Default to unknown
    if (p->type == 0x90) {
        stat_packet_type = 0x0D; // METER_PUSH packet type
    } else if (p->type == 0x91) {
        stat_packet_type = 0x0F; // HEARTBEAT packet type
    } else if (p->type == 0x31) {
        stat_packet_type = 0x31; // BROWSE_RESP packet type
    }

    if (p->type == 0x90 && p->len >= 8) {
        uint16_t q_rmsL = (p->payload[0] | (p->payload[1]<<8));
        uint16_t q_rmsR = (p->payload[2] | (p->payload[3]<<8));
        uint16_t q_pkL  = (p->payload[4] | (p->payload[5]<<8));
        uint16_t q_pkR  = (p->payload[6] | (p->payload[7]<<8));
        
        // Convert Q15 to float (divide by 32767.0f)
        float rms_left = (float)q_rmsL / 32767.0f;
        float rms_right = (float)q_rmsR / 32767.0f;
        float peak_left = (float)q_pkL / 32767.0f;
        float peak_right = (float)q_pkR / 32767.0f;
        
        #if WAVEX_MCU_LINK_PACKET_DEBUG
        ESP_LOGI(TAG, "METER L=%.3f R=%.3f PEAK_L=%.3f PEAK_R=%.3f", rms_left, rms_right, peak_left, peak_right);
        #endif
        inter_mcu_update_backend_meters(rms_left, rms_right, peak_left, peak_right);
    } else if (p->type == 0x91 && p->len >= 12) {
        uint32_t uptime = (uint32_t)p->payload[0] | ((uint32_t)p->payload[1] << 8) |
                          ((uint32_t)p->payload[2] << 16) | ((uint32_t)p->payload[3] << 24);
        uint32_t loop_counter = (uint32_t)p->payload[4] | ((uint32_t)p->payload[5] << 8) |
                                ((uint32_t)p->payload[6] << 16) | ((uint32_t)p->payload[7] << 24);
        uint32_t rx_total = (uint32_t)p->payload[8] | ((uint32_t)p->payload[9] << 8) |
                            ((uint32_t)p->payload[10] << 16) | ((uint32_t)p->payload[11] << 24);
        
        // Extract CPU usage if available (extended heartbeat)
        float cpu_usage_percent = 0.0f;
        if (p->len >= 14) {
            uint16_t cpu_usage_scaled = (uint16_t)p->payload[12] | ((uint16_t)p->payload[13] << 8);
            cpu_usage_percent = (float)cpu_usage_scaled / 10.0f; // Convert back from scaled integer
        }
        
        inter_mcu_update_backend_heartbeat(uptime, rx_total, loop_counter, cpu_usage_percent);
    }
    // Note: Browse responses (0x31) are now handled as large packets, not control packets
    
    // Update packet statistics (outside any critical sections)
    inter_mcu_increment_packet_stat(stat_packet_type);
#else
    (void)p;
#endif
}

// ============================================================================
// ESP32 to Daisy Message Queue
// ============================================================================

#define MSG_QUEUE_SIZE 8
typedef struct {
    uint16_t type;
    uint16_t len;
    uint8_t payload[32];
    uint8_t seq_num;
    bool pending;
} msg_queue_entry_t;

// TX queue for messages to send TO Daisy
static msg_queue_entry_t msg_queue[MSG_QUEUE_SIZE];
static volatile int msg_queue_head = 0;
static volatile int msg_queue_tail = 0;
static volatile int msg_queue_count = 0;
static uint8_t next_seq_num = 1; // Sequence number for message tracking (0 reserved for no message)

// Mutex for protecting queue operations
static portMUX_TYPE s_spi_mutex = portMUX_INITIALIZER_UNLOCKED;


// Helper function to calculate CRC over header + payload (excluding CRC field)
static uint16_t calculate_packet_crc(const ctrl_pkt_t* pkt)
{
    uint8_t crc_data[4 + 20]; // Header (4) + max payload (20)
    crc_data[0] = pkt->type;
    crc_data[1] = pkt->flags;
    crc_data[2] = pkt->seq;
    crc_data[3] = pkt->len;
    memcpy(&crc_data[4], pkt->payload, pkt->len);
    
    // Debug: Log CRC calculation data
    ESP_LOGI(TAG, "ESP32 CRC calculation: %d bytes - %02X %02X %02X %02X %02X", 
             4 + pkt->len, crc_data[0], crc_data[1], crc_data[2], crc_data[3], crc_data[4]);
    
    uint16_t crc = wavex_crc16(crc_data, 4 + pkt->len);
    ESP_LOGI(TAG, "ESP32 calculated CRC: 0x%04X", crc);
    return crc;
}

// Signal Daisy for urgent control data (active high on GPIO31)
static void signal_daisy_urgent(bool urgent)
{
#ifdef ESP_PLATFORM
    gpio_set_level((gpio_num_t)WAVEX_ESP_ATTN_OUT, urgent ? 1 : 0);  // Active high
    if (urgent) {
        ESP_LOGI(TAG, "Signaling Daisy for urgent control (GPIO31 HIGH) - queue_count=%d", msg_queue_count);
    } else {
        ESP_LOGI(TAG, "Cleared Daisy urgent signal (GPIO31 LOW) - queue_count=%d", msg_queue_count);
    }
    
    // Debug: Read back the GPIO level to verify it was set correctly
    int current_level = gpio_get_level((gpio_num_t)WAVEX_ESP_ATTN_OUT);
    ESP_LOGI(TAG, "GPIO31 readback: %s (expected: %s)", 
             current_level ? "HIGH" : "LOW", 
             urgent ? "HIGH" : "LOW");
#endif
}

// Prepare TX buffer with queued message without consuming from queue
static void prepare_tx_buffer_without_consuming(uint8_t* tx_buf, size_t len)
{
    // Always clear the response buffer first
    if (tx_buf && len > 0) {
        memset(tx_buf, 0, len);
    }
    
    // Enter critical section to protect queue operations
    taskENTER_CRITICAL(&s_spi_mutex);
    
    // Check if we have messages to send
    if (msg_queue_count == 0) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGI(TAG, "No messages to send, preparing no-data response");
        // Send a proper "no data" packet instead of all zeros
        ctrl_pkt_t* pkt = (ctrl_pkt_t*)tx_buf;
        pkt->type = 0x01;  // Command packet type
        pkt->flags = 0;
        pkt->seq = 0;
        pkt->len = 1;      // Just the message type
        pkt->payload[0] = WaveX::Protocol::MSG_ACK; // Use ACK as "no data" indicator
        pkt->crc = calculate_packet_crc(pkt);
        ESP_LOGI(TAG, "Prepared no-data packet: type=0x%02X, len=%d, payload[0]=0x%02X, crc=0x%04X", 
                 pkt->type, pkt->len, pkt->payload[0], pkt->crc);
        ESP_LOGI(TAG, "No-data packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                 ((uint8_t*)pkt)[0], ((uint8_t*)pkt)[1], ((uint8_t*)pkt)[2], ((uint8_t*)pkt)[3],
                 ((uint8_t*)pkt)[4], ((uint8_t*)pkt)[5], ((uint8_t*)pkt)[6], ((uint8_t*)pkt)[7]);
        return;
    }
    
    // Get the next message from queue (with bounds checking) - DON'T consume it yet
    int idx = msg_queue_head;
    if (idx >= MSG_QUEUE_SIZE) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGE(TAG, "Invalid queue head index: %d", idx);
        return;
    }
    
    msg_queue_entry_t* entry = &msg_queue[idx];
    if (!entry->pending) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGD(TAG, "Message at head not pending, preparing zeros");
        return;
    }
    
    // Copy message data before exiting critical section
    uint16_t msg_type = entry->type;
    uint16_t msg_len = entry->len;
    uint8_t msg_payload[32];
    if (msg_len > 0 && msg_len <= sizeof(msg_payload)) {
        memcpy(msg_payload, entry->payload, msg_len);
    }
    
    taskEXIT_CRITICAL(&s_spi_mutex);
    
    // Prepare response packet - ensure we don't exceed buffer size
    size_t response_size = sizeof(ctrl_pkt_t);
    if (len < response_size) {
        ESP_LOGE(TAG, "TX buffer too small: %d < %d", (int)len, (int)response_size);
        return;
    }
    
    ctrl_pkt_t* pkt = (ctrl_pkt_t*)tx_buf;
    memset(pkt, 0, response_size);
    
    // New format: packet type is 0x01 (command), message type goes in payload
    pkt->type = 0x01;  // Command packet type
    pkt->flags = 0;
    pkt->seq = 0;
    pkt->len = msg_len + 1;  // +1 for message type byte
    
    // First byte of payload is the message type
    pkt->payload[0] = msg_type & 0xFF;
    
    // Copy actual payload after message type
    if (msg_len > 0 && msg_len <= sizeof(pkt->payload) - 1) {
        memcpy(&pkt->payload[1], msg_payload, msg_len);
    }
    
    // Calculate CRC over header + payload
    pkt->crc = calculate_packet_crc(pkt);
    
    ESP_LOGI(TAG, "Prepared TX buffer: type=0x%02X, len=%d, msg_type=0x%02X, payload=%.*s", 
             pkt->type, pkt->len, msg_type & 0xFF, (int)msg_len, msg_payload);
    ESP_LOGI(TAG, "TX buffer: %02X %02X %02X %02X %02X %02X %02X %02X", 
             tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3], tx_buf[4], tx_buf[5], tx_buf[6], tx_buf[7]);
}

// Clear message from TX queue after successful transmission to Daisy (legacy - for non-ACK messages)
static void clear_transmitted_message_from_queue(void)
{
    // Enter critical section to protect queue operations
    taskENTER_CRITICAL(&s_spi_mutex);
    
    if (msg_queue_count == 0) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        // ESP_LOGW(TAG, "No messages in TX queue to clear");
        return;
    }
    
    // Get the next message from queue (with bounds checking)
    int idx = msg_queue_head;
    if (idx >= MSG_QUEUE_SIZE) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGE(TAG, "Invalid queue head index: %d", idx);
        return;
    }
    
    msg_queue_entry_t* entry = &msg_queue[idx];
    if (!entry->pending) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGW(TAG, "Message at head not pending");
        return;
    }
    
    // Mark message as transmitted and remove from queue
    entry->pending = false;
    msg_queue_head = (msg_queue_head + 1) % MSG_QUEUE_SIZE;
    msg_queue_count--;
    
    // Clear attention signal if queue is now empty
    bool queue_empty = (msg_queue_count == 0);
    
    taskEXIT_CRITICAL(&s_spi_mutex);
    
    if (queue_empty) {
        // Clear urgent signal when all messages are sent
        signal_daisy_urgent(false);
        ESP_LOGI(TAG, "TX queue empty, cleared urgent signal");
    }
    
    ESP_LOGD(TAG, "Cleared transmitted message from TX queue, remaining: %d", msg_queue_count);
}

// Handle SPI slave response - called when Daisy polls us
static void handle_spi_slave_response(uint8_t* tx_buf, uint8_t* rx_buf, size_t len)
{
    // Always clear the response buffer first
    if (tx_buf && len > 0) {
        memset(tx_buf, 0, len);
    }
    
    // Enter critical section to protect queue operations
    taskENTER_CRITICAL(&s_spi_mutex);
    
    // Check if we have messages to send
    if (msg_queue_count == 0) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGI(TAG, "No messages to send, preparing no-data response");
        // Send a proper "no data" packet instead of all zeros
        ctrl_pkt_t* pkt = (ctrl_pkt_t*)tx_buf;
        pkt->type = 0x01;  // Command packet type
        pkt->flags = 0;
        pkt->seq = 0;
        pkt->len = 1;      // Just the message type
        pkt->payload[0] = WaveX::Protocol::MSG_ACK; // Use ACK as "no data" indicator
        pkt->crc = calculate_packet_crc(pkt);
        ESP_LOGI(TAG, "Prepared no-data packet: type=0x%02X, len=%d, payload[0]=0x%02X, crc=0x%04X", 
                 pkt->type, pkt->len, pkt->payload[0], pkt->crc);
        ESP_LOGI(TAG, "No-data packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                 ((uint8_t*)pkt)[0], ((uint8_t*)pkt)[1], ((uint8_t*)pkt)[2], ((uint8_t*)pkt)[3],
                 ((uint8_t*)pkt)[4], ((uint8_t*)pkt)[5], ((uint8_t*)pkt)[6], ((uint8_t*)pkt)[7]);
        return;
    }
    
    // Get the next message from queue (with bounds checking)
    int idx = msg_queue_head;
    if (idx >= MSG_QUEUE_SIZE) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGE(TAG, "Invalid queue head index: %d", idx);
        return;
    }
    
    msg_queue_entry_t* entry = &msg_queue[idx];
    if (!entry->pending) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGI(TAG, "Message at head not pending, preparing no-data response");
        // Send a proper "no data" packet instead of all zeros
        ctrl_pkt_t* pkt = (ctrl_pkt_t*)tx_buf;
        pkt->type = 0x01;  // Command packet type
        pkt->flags = 0;
        pkt->seq = 0;
        pkt->len = 1;      // Just the message type
        pkt->payload[0] = WaveX::Protocol::MSG_ACK; // Use ACK as "no data" indicator
        pkt->crc = calculate_packet_crc(pkt);
        ESP_LOGI(TAG, "Prepared no-data packet: type=0x%02X, len=%d, payload[0]=0x%02X, crc=0x%04X", 
                 pkt->type, pkt->len, pkt->payload[0], pkt->crc);
        return;
    }
    
    // Prepare response packet - ensure we don't exceed buffer size
    size_t response_size = sizeof(ctrl_pkt_t);
    if (len < response_size) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGE(TAG, "TX buffer too small: %d < %d", (int)len, (int)response_size);
        return;
    }
    
    // Copy message data before modifying queue
    uint16_t msg_type = entry->type;
    uint16_t msg_len = entry->len;
    uint8_t msg_payload[32];
    if (msg_len > 0 && msg_len <= sizeof(msg_payload)) {
        memcpy(msg_payload, entry->payload, msg_len);
    }
    
    // Mark message as processed and remove from queue
    entry->pending = false;
    msg_queue_head = (msg_queue_head + 1) % MSG_QUEUE_SIZE;
    msg_queue_count--;
    
    taskEXIT_CRITICAL(&s_spi_mutex);
    
    // Now prepare the response packet (outside critical section)
    ctrl_pkt_t* pkt = (ctrl_pkt_t*)tx_buf;
    memset(pkt, 0, response_size);
    
    // New format: packet type is 0x01 (command), message type goes in payload
    pkt->type = 0x01;  // Command packet type
    pkt->flags = 0;
    pkt->seq = 0;
    pkt->len = msg_len + 1;  // +1 for message type byte
    
    // First byte of payload is the message type
    pkt->payload[0] = msg_type & 0xFF;
    
    // Copy actual payload after message type
    if (msg_len > 0 && msg_len <= sizeof(pkt->payload) - 1) {
        memcpy(&pkt->payload[1], msg_payload, msg_len);
    }
    
    // Calculate CRC over header + payload
    pkt->crc = calculate_packet_crc(pkt);
    
    ESP_LOGI(TAG, "Responding to poll with message type=0x%02X, len=%d, payload[0]=0x%02X, crc=0x%04X", 
             pkt->type, pkt->len, pkt->payload[0], pkt->crc);
    ESP_LOGI(TAG, "Response packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
             ((uint8_t*)pkt)[0], ((uint8_t*)pkt)[1], ((uint8_t*)pkt)[2], ((uint8_t*)pkt)[3],
             ((uint8_t*)pkt)[4], ((uint8_t*)pkt)[5], ((uint8_t*)pkt)[6], ((uint8_t*)pkt)[7]);
}

int spi_link_send(uint16_t type, const void* payload, uint16_t len)
{
    if (len > sizeof(msg_queue[0].payload)) {
        ESP_LOGE(TAG, "Message payload too large: %d > %d", len, (int)sizeof(msg_queue[0].payload));
        return 0;
    }

    // Enter critical section to protect queue operations
    taskENTER_CRITICAL(&s_spi_mutex);

    if (msg_queue_count >= MSG_QUEUE_SIZE) {
        taskEXIT_CRITICAL(&s_spi_mutex);
        ESP_LOGW(TAG, "Message queue full, dropping message type 0x%04X", type);
        return 0;
    }

    // Add message to queue
    int idx = msg_queue_tail;
    msg_queue[idx].type = type;
    msg_queue[idx].len = len;
    if (payload && len > 0) {
        memcpy(msg_queue[idx].payload, payload, len);
    }
    msg_queue[idx].seq_num = next_seq_num++;
    if (next_seq_num == 0) next_seq_num = 1; // Skip 0, reserved for "no message"
    msg_queue[idx].pending = true;

    msg_queue_tail = (msg_queue_tail + 1) % MSG_QUEUE_SIZE;
    msg_queue_count++;

    // Snapshot queue state for logging outside the critical section
    int snapshot_head = msg_queue_head;
    int snapshot_tail = msg_queue_tail;
    int snapshot_count = msg_queue_count;

    taskEXIT_CRITICAL(&s_spi_mutex);

    ESP_LOGI(TAG, "Queued message type 0x%04X, len=%d, queue_count=%d", type, len, msg_queue_count);
    ESP_LOGI(TAG, "Queued message payload: %.*s", (int)len, (const char*)payload);
    ESP_LOGI(TAG, "DEBUG: After queuing - head=%d, tail=%d, count=%d", snapshot_head, snapshot_tail, snapshot_count);
    
    // Special logging for BROWSE_REQ
    if (type == WaveX::Protocol::MSG_BROWSE_REQ) {
        ESP_LOGI(TAG, "*** BROWSE_REQ QUEUED SUCCESSFULLY ***");
    }
    
    // Signal Daisy for urgent response if this is a control message
    if (type == WaveX::Protocol::MSG_CONTROL_CHANGE || 
        type == WaveX::Protocol::MSG_NOTE_ON ||
        type == WaveX::Protocol::MSG_NOTE_OFF ||
        type == WaveX::Protocol::MSG_BROWSE_REQ ||
        type == WaveX::Protocol::MSG_SAMPLE_PLAY_REQ ||
        type == WaveX::Protocol::MSG_SAMPLE_STOP_REQ) {
        signal_daisy_urgent(true);
        ESP_LOGI(TAG, "URGENT control message queued, signaling Daisy");
    }
    
    ESP_LOGI(TAG, "Message queued in TX queue");
    ESP_LOGI(TAG, "*** TX MESSAGE QUEUED - COUNT: %d ***", msg_queue_count);
    
    return 1;
}

// get_next_message function removed - using direct response in SPI slave handler

#if WAVEX_SPI_LINK_ENABLED && defined(ESP_PLATFORM)

// -----------------------------
// Stats (optional)
// -----------------------------
static spi_link_stats_t s_stats = {0, 0, 0, 0, 0};

// -----------------------------
// HD queues, buffers, descs
// -----------------------------
#ifndef WAVEX_ESP_SPI_HOST
#define WAVEX_ESP_SPI_HOST  SPI3_HOST   // keep link on SPI3; keep ADC/LED on SPI2 master
#endif

// You already defined these in your board header:
#ifndef WAVEX_ESP_SPI_SCLK
#  error "Define WAVEX_ESP_SPI_SCLK / MOSI / MISO / CS in link_config.h or board header"
#endif

// Depths
#define RX_DESC_COUNT  4        // how many RX DMA descriptors
#define TX_DESC_COUNT  2        // double-buffer TX so we can update safely

// DMA-capable buffers (aligned, internal)
static uint8_t *rxbuf[RX_DESC_COUNT];
static uint8_t *txbuf[TX_DESC_COUNT];

static spi_slave_transaction_t rx_trans[RX_DESC_COUNT];
static spi_slave_transaction_t tx_trans[TX_DESC_COUNT];

static volatile uint8_t tx_seq_echo = 0;
// s_next_tx_desc_idx removed - not needed for simple polling

// Track whether the queued RX transaction was prepared with a message payload
static bool rx_trans_had_message[RX_DESC_COUNT] = { false, false, false, false };

// -----------------------------
// Helpers
// -----------------------------
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void make_resp_packet(ctrl_pkt_t *out, uint8_t echo_seq)
{
    memset(out, 0, sizeof(*out));
    out->type = 0x81;   // RESP
    out->seq  = echo_seq;
    out->len  = 0;
    out->crc  = wavex_crc16((const uint8_t*)out, 30);
}

// -----------------------------
// Init
// -----------------------------
esp_err_t spi_link_init(void)
{
    ESP_LOGI(TAG, "Initializing Regular SPI Slave");

    // Configure GPIO31 → Daisy D0 attention signal (ESP32 signals when it has urgent control)
    gpio_config_t attn_config = {
        .pin_bit_mask = (1ULL << WAVEX_ESP_ATTN_OUT),  // GPIO31
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&attn_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure attention GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize attention line to low (inactive)
    gpio_set_level((gpio_num_t)WAVEX_ESP_ATTN_OUT, 0);
    
    ESP_LOGI(TAG, "Attention GPIO%d configured for real-time control signaling", WAVEX_ESP_ATTN_OUT);

    // Allocate DMA-capable buffers - RX buffers need to be large for big packets
    for (int i=0;i<RX_DESC_COUNT;i++) {
        rxbuf[i] = (uint8_t*)heap_caps_aligned_alloc(4, MAX_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!rxbuf[i]) return ESP_ERR_NO_MEM;
        memset(rxbuf[i], 0, MAX_PKT_SIZE);
    }
    // TX buffers can remain small for control packets
    for (int i=0;i<TX_DESC_COUNT;i++) {
        txbuf[i] = (uint8_t*)heap_caps_aligned_alloc(4, CTRL_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!txbuf[i]) return ESP_ERR_NO_MEM;
        memset(txbuf[i], 0, CTRL_PKT_SIZE);
        make_resp_packet((ctrl_pkt_t*)txbuf[i], 0);
    }

    // Bus config (pins via matrix)
    spi_bus_config_t bus = {
        .mosi_io_num     = WAVEX_ESP_SPI_MOSI,
        .miso_io_num     = WAVEX_ESP_SPI_MISO,
        .sclk_io_num     = WAVEX_ESP_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = CTRL_PKT_SIZE,
        .flags           = SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MISO | SPICOMMON_BUSFLAG_GPIO_PINS,
    };

    // Regular SPI slave config — Mode 0
    spi_slave_interface_config_t slave_config = {
        .spics_io_num = WAVEX_ESP_SPI_CS,
        .flags        = 0,
        .queue_size   = RX_DESC_COUNT,
        .mode         = 0,  // CPOL=0, CPHA=0
        .post_setup_cb = NULL,
        .post_trans_cb = NULL
    };

    // Initialize SPI bus
    esp_err_t r = spi_slave_initialize(WAVEX_ESP_SPI_HOST, &bus, &slave_config, SPI_DMA_CH_AUTO);
    ESP_LOGI(TAG, "spi_slave_initialize -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return r;

    // Initialize transaction structures
    for (int i=0;i<RX_DESC_COUNT;i++) {
        memset(&rx_trans[i], 0, sizeof(spi_slave_transaction_t));
        rx_trans[i].rx_buffer = rxbuf[i];
        rx_trans[i].length = MAX_PKT_SIZE * 8;  // Length in bits - support large packets
    }

    for (int i=0;i<TX_DESC_COUNT;i++) {
        memset(&tx_trans[i], 0, sizeof(spi_slave_transaction_t));
        tx_trans[i].tx_buffer = txbuf[i];
        tx_trans[i].length = CTRL_PKT_SIZE * 8;  // Length in bits
    }

    ESP_LOGI(TAG, "Regular SPI slave ready. Pins: SCK=%d MOSI=%d MISO=%d CS=%d  (Mode0)",
            WAVEX_ESP_SPI_SCLK, WAVEX_ESP_SPI_MOSI, WAVEX_ESP_SPI_MISO, WAVEX_ESP_SPI_CS);
    return ESP_OK;
}

// -----------------------------
// Link task (waits for SPI transactions)
// -----------------------------
static void link_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Regular SPI Slave link task start");
    uint32_t last_log = now_ms();
    int current_rx_idx = 0;

    while (true) {
        // Prepare TX buffer with queued message before queuing RX transaction
        spi_slave_transaction_t *rx_trans_ptr = &rx_trans[current_rx_idx];
        
        // Always clear TX buffer first
        memset(txbuf[0], 0, CTRL_PKT_SIZE);
        
        // Set up RX transaction (always ready to receive MAX_PKT_SIZE)
        rx_trans_ptr->rx_buffer = rxbuf[current_rx_idx];
        rx_trans_ptr->length = MAX_PKT_SIZE * 8;  // RX length in bits - always max size
        
        // Debug: Check attention signal state
        static uint32_t debug_attn_count = 0;
        if (++debug_attn_count % 1000 == 0) {
            ESP_LOGI(TAG, "ESP32 attention signal state: queue_count=%d", msg_queue_count);
        }
        
        // Debug: Log when we have messages in queue
        if (msg_queue_count > 0) {
            ESP_LOGI(TAG, "ESP32 has %d messages in queue, head=%d, tail=%d", 
                     msg_queue_count, msg_queue_head, msg_queue_tail);
        }
        
        // Prepare TX buffer with current message (if any)
        if (msg_queue_count > 0) {
            // We have a message to send - prepare TX buffer with current message
            prepare_tx_buffer_without_consuming((uint8_t*)txbuf[0], CTRL_PKT_SIZE);
            rx_trans_ptr->tx_buffer = txbuf[0];
            rx_trans_had_message[current_rx_idx] = true;
            ESP_LOGI(TAG, "Prepared TX buffer with queued message, queue_count=%d", msg_queue_count);
        } else {
            // No message - prepare proper no-data response
            prepare_tx_buffer_without_consuming((uint8_t*)txbuf[0], CTRL_PKT_SIZE);
            rx_trans_ptr->tx_buffer = txbuf[0];
            rx_trans_had_message[current_rx_idx] = false;
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            ESP_LOGI(TAG, "Prepared no-data TX buffer");
            #endif
        }
        
        // Queue RX transaction
        esp_err_t r = spi_slave_queue_trans(WAVEX_ESP_SPI_HOST, rx_trans_ptr, portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "spi_slave_queue_trans failed: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Wait for transaction to complete
        spi_slave_transaction_t *completed_trans = NULL;
        r = spi_slave_get_trans_result(WAVEX_ESP_SPI_HOST, &completed_trans, pdMS_TO_TICKS(3000));
        if (r == ESP_ERR_TIMEOUT) {
            // Log periodically to show we're waiting for SPI transactions
            uint32_t now = now_ms();
            if (now - last_log > 5000) {
                #if WAVEX_MCU_LINK_PACKET_DEBUG
                ESP_LOGI(TAG, "Waiting for SPI transactions from master...");
                #endif
                last_log = now;
            }
            continue;
        } else if (r != ESP_OK || !completed_trans) {
            ESP_LOGW(TAG, "spi_slave_get_trans_result -> %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else {
            // A transaction was received!
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            ESP_LOGI(TAG, "SPI RX transaction completed, len=%d bits", completed_trans->trans_len);
            #endif
            
            // Determine which RX descriptor completed
            int completed_idx = -1;
            for (int i = 0; i < RX_DESC_COUNT; i++) {
                if (completed_trans == &rx_trans[i]) { completed_idx = i; break; }
            }

            // Check if this was a poll request (0xFF) and we actually transmitted a message in this transaction.
            if (completed_trans->rx_buffer && completed_idx >= 0) {
                uint8_t* rx_data = (uint8_t*)completed_trans->rx_buffer;
                if (rx_data[0] == 0xFF && rx_trans_had_message[completed_idx]) {
                    // Clear the message from TX queue only if this transaction carried a message
                    if (msg_queue_count > 0) {
                        clear_transmitted_message_from_queue();
                    }
                }
                // Reset flag for this descriptor
                rx_trans_had_message[completed_idx] = false;
            }
            
            // Auto-detect packet type and process accordingly
            uint8_t* rx_data = (uint8_t*)completed_trans->rx_buffer;
            size_t rx_len = completed_trans->trans_len / 8; // Convert bits to bytes
            
            ESP_LOGI(TAG, "Received %d bytes, first 4 bytes: %02X %02X %02X %02X",
                     (int)rx_len, rx_data[0], rx_data[1], rx_data[2], rx_data[3]);
            
            // Debug: If we receive more than 4 bytes, show more data for debugging
            if (rx_len > 4) {
                ESP_LOGI(TAG, "Large reception - bytes 4-7: %02X %02X %02X %02X", 
                         rx_data[4], rx_data[5], rx_data[6], rx_data[7]);
            }
            
            // Handle zero-length transactions - but check if we have valid data anyway
            if (rx_len == 0) {
                ESP_LOGW(TAG, "Received 0-byte transaction - SPI timing or CS issue");
                
                // Check if we actually have valid data despite the 0 length report
                // This can happen with SPI timing issues where data arrives but length isn't detected
                bool has_actual_data = false;
                for (int i = 0; i < CTRL_PKT_SIZE && i < 8; i++) { // Check first 8 bytes
                    if (rx_data[i] != 0) {
                        has_actual_data = true;
                        break;
                    }
                }
                
                if (has_actual_data) {
                    // Check if this might be a large packet by examining the header
                    if (rx_data[0] == WaveX::Protocol::SYNC_BYTE) {
                        // This is a large packet - calculate actual size from header
                        uint8_t payload_len = rx_data[2]; // Length field in PacketHeader
                        size_t expected_packet_size = 4 + payload_len; // header + payload
                        if (expected_packet_size <= MAX_PKT_SIZE) {
                            rx_len = expected_packet_size;
                            ESP_LOGI(TAG, "Found large packet despite 0-length report, assuming %d bytes (header says %d+4)", 
                                     (int)rx_len, payload_len);
                        } else {
                            rx_len = MAX_PKT_SIZE; // Cap at maximum
                            ESP_LOGI(TAG, "Found oversized packet, capping at %d bytes", MAX_PKT_SIZE);
                        }
                    } else {
                        // Regular control packet
                        rx_len = CTRL_PKT_SIZE; // Assume standard control packet size
                        ESP_LOGI(TAG, "Found valid data despite 0-length report, assuming %d bytes", CTRL_PKT_SIZE);
                    }
                    // Log this as a timing issue that should be investigated
                    ESP_LOGW(TAG, "SPI timing issue detected - data present but length=0");
                } else {
                    continue; // Skip processing if truly no data
                }
            }
            
            // Detect packet type by examining the first few bytes
            if (rx_len >= 5 && rx_data[0] == 0x01 && rx_data[3] == 0x01 && rx_data[4] == 0xFF) {
                // This is a poll request from Daisy - we already prepared response in TX buffer
                ESP_LOGI(TAG, "Detected poll request (0xFF in payload) from Daisy");
                // No additional processing needed - response was already prepared
            } else if (rx_len >= 4 && rx_data[0] == 0x00 && rx_data[1] == 0x00 && rx_data[2] == 0x00 && rx_data[3] == 0x00) {
                // This is an empty packet from Daisy requesting urgent data
                ESP_LOGI(TAG, "Detected urgent data request from Daisy (empty packet)");
                // ESP32 should respond with queued data - this is handled by the TX buffer preparation
                // Clear ATTN line after responding
                if (msg_queue_count > 0) {
                    clear_transmitted_message_from_queue();
                }
            } else if (rx_len >= 4 && rx_data[0] == WaveX::Protocol::SYNC_BYTE) {
                // Large packet format (pkt_t) - starts with sync byte 0xAA
                ESP_LOGI(TAG, "Detected large packet format (pkt_t)");
                handle_large_packet(rx_data, rx_len);
            } else if (rx_len >= 4 && rx_len <= 64) {
                // Small control packet format (ctrl_pkt_t) - could be control messages from Daisy
                // ESP_LOGI(TAG, "Detected control packet format (ctrl_pkt_t)");
                ctrl_pkt_t *rxp = (ctrl_pkt_t*)rx_data;
                
                // Check if this is a new format command packet from Daisy (type=0x01)
                if (rxp->type == 0x01 && rxp->len > 0) {
                    // Check if this is a poll request (0xFF in payload)
                    if (rxp->payload[0] == 0xFF) {
                        ESP_LOGI(TAG, "Received poll request from Daisy - no processing needed");
                        // No processing needed for poll requests
                        continue; // Skip to next iteration
                    } else {
                        // New format: message type is in first byte of payload
                        uint8_t msg_type = rxp->payload[0];
                        uint8_t msg_len = rxp->len - 1; // Subtract message type byte
                        const uint8_t* msg_payload = &rxp->payload[1];
                        
                        ESP_LOGI(TAG, "Processing new format command packet: msg_type=0x%02X, len=%d", msg_type, msg_len);
                        handle_control_message_from_daisy_new_format(msg_type, msg_payload, msg_len);
                    }
                } else {
                    // Handle as regular packet (meter data from backend, etc.)
                    handle_ctrl_packet(rxp);
                }
            } else {
                ESP_LOGW(TAG, "Unknown packet format, rx_len=%d", (int)rx_len);
            }
            
            s_stats.packets_received++;
            
            // Move to next RX buffer
            current_rx_idx = (current_rx_idx + 1) % RX_DESC_COUNT;
        }

        // Occasional alive log
        uint32_t now = now_ms();
        if (now - last_log > 10000) {
            ESP_LOGI(TAG, "Alive: RX=%lu CRCerr=%lu",
                     (unsigned long)s_stats.packets_received,
                     (unsigned long)s_stats.crc_errors);
            last_log = now;
        }
    }
}

// -----------------------------
// Public API
// -----------------------------
esp_err_t spi_link_start(void)
{
    ESP_LOGI(TAG, "Starting SPI link task (HD)");
    BaseType_t ok = xTaskCreate(link_task, "spi_hd_rx", 8192, NULL, 20, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool spi_link_is_active(void)
{
    return (s_stats.irq_count > 0) || (s_stats.packets_received > 0);
}

void spi_link_get_stats(spi_link_stats_t *pstats)
{
    if (pstats) memcpy(pstats, &s_stats, sizeof(s_stats));
}

#endif // WAVEX_SPI_LINK_ENABLED && ESP_PLATFORM

#ifdef __cplusplus
}
#endif
