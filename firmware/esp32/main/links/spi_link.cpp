#include "spi_link.h"
#include "spi_protocol/spi_protocol.h"
#include "link_config.h"
#include "../inter_mcu.h"
#include "../comm/packet_router.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/config/logging_config.h"
#include "../../shared/config/pin_config.h"
#include "hardware_pins.h"
#include <string.h>
#include <assert.h>

using namespace WaveX::Protocol;

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
#include "esp_intr_alloc.h"
#include "soc/spi_periph.h"
#include "../../shared/config/logging_config.h"
#endif

// -----------------------------
// Packet & CRC
// -----------------------------
static const char *TAG = "spi_link";

// Logging component identifier
#define ESP32_INTER_SPI 1

#define CTRL_PKT_SIZE PKT_SIZE_32
#define MAX_PKT_SIZE 2048  // Support up to 2KB packets

// Use new unified packet system
#define wavex_crc16 ProtocolHandler::CalculateWaveXCrc
#define validate_wave_packet ProtocolHandler::ValidateWaveXPacket
#define create_wave_packet ProtocolHandler::CreateWaveXPacket
#define parse_wave_packet ProtocolHandler::ParseWaveXPacket

// Global packet router instance
static WaveX::Comm::PacketRouter s_packet_router;

// SPI slave transaction buffers - DMA-capable and aligned
// Use triple buffering for efficient packet processing
#define BUFFER_POOL_SIZE 3
static uint8_t* s_rx_buffers[BUFFER_POOL_SIZE];
static uint8_t* s_tx_buffers[BUFFER_POOL_SIZE];
static spi_slave_transaction_t s_transactions[BUFFER_POOL_SIZE];
static int s_current_tx_index = 0;
static int s_current_rx_index = 0;
static int s_processing_index = -1; // Index of buffer being processed (-1 = none)

// Track whether the last received packet was a one-way packet
static bool s_last_packet_was_one_way = false;

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

// Forward declarations
static void spi_slave_task(void* pvParameters);
static esp_err_t allocate_dma_buffers(void);
static void free_dma_buffers(void);

// Initialize packet router
static void init_packet_router()
{
    s_packet_router.set_stats_callback([](uint8_t packet_type) {
        inter_mcu_increment_packet_stat(packet_type);
    });
}

// Allocate DMA-capable buffers
static esp_err_t allocate_dma_buffers(void)
{
    ESP_LOGI(TAG, "Allocating DMA-capable buffers");
    
    // Allocate DMA-capable RX buffers - 2048 bytes each for triple buffering
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        s_rx_buffers[i] = (uint8_t*)heap_caps_aligned_alloc(64, MAX_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_rx_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate RX buffer %d", i);
            free_dma_buffers();
            return ESP_ERR_NO_MEM;
        }
        memset(s_rx_buffers[i], 0, MAX_PKT_SIZE);
        ESP_LOGD(TAG, "Allocated RX buffer %d at %p", i, s_rx_buffers[i]);
    }
    
    // Allocate DMA-capable TX buffers - 2048 bytes each for triple buffering
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        s_tx_buffers[i] = (uint8_t*)heap_caps_aligned_alloc(64, MAX_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_tx_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate TX buffer %d", i);
            free_dma_buffers();
            return ESP_ERR_NO_MEM;
        }
        memset(s_tx_buffers[i], 0, MAX_PKT_SIZE);
        ESP_LOGD(TAG, "Allocated TX buffer %d at %p", i, s_tx_buffers[i]);
    }
    
    ESP_LOGI(TAG, "DMA buffers allocated successfully (%d buffers, %d bytes each)", BUFFER_POOL_SIZE, MAX_PKT_SIZE);
    return ESP_OK;
}

// Free DMA-capable buffers
static void free_dma_buffers(void)
{
    ESP_LOGI(TAG, "Freeing DMA-capable buffers");
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (s_rx_buffers[i]) {
            free(s_rx_buffers[i]);
            s_rx_buffers[i] = nullptr;
        }
        if (s_tx_buffers[i]) {
            free(s_tx_buffers[i]);
            s_tx_buffers[i] = nullptr;
        }
    }
}

// Signal Daisy for urgent control data (active high on GPIO31)
static void signal_daisy_urgent(bool urgent)
{
#ifdef ESP_PLATFORM
    gpio_set_level((gpio_num_t)WAVEX_INTER_MCU_GPIO_ATTN, urgent ? 1 : 0);  // Active high
    if (urgent) {
        WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "Signaling Daisy for urgent control (GPIO31 HIGH) - queue_count=%d", msg_queue_count);
    } else {
        WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "Cleared Daisy urgent signal (GPIO31 LOW) - queue_count=%d", msg_queue_count);
    }
    
    // Debug: Read back the GPIO level to verify it was set correctly
    int current_level = gpio_get_level((gpio_num_t)WAVEX_INTER_MCU_GPIO_ATTN);
    WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "GPIO31 readback: %s (expected: %s)", 
             current_level ? "HIGH" : "LOW", 
             urgent ? "HIGH" : "LOW");
#endif
}


// Handle large packet format - route to packet router
static void handle_large_packet(const uint8_t* packet_data, size_t packet_len)
{
#ifdef ESP_PLATFORM
    if (packet_len < 6) { // Minimum size for unified packet (4 header + 2 CRC)
        ESP_LOGE(TAG, "Large packet too short: %d bytes", (int)packet_len);
        return;
    }

    // Validate unified packet
    if (!validate_wave_packet(packet_data, packet_len)) {
        ESP_LOGE(TAG, "Large packet CRC validation failed");
        return;
    }

    // Extract packet info using unified format
    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[MAX_PAYLOAD_SIZE]; // Max payload size
    size_t payload_size;

    if (!parse_wave_packet(packet_data, packet_len, msg_type, payload, payload_size, sequence_number, flags)) {
        ESP_LOGE(TAG, "Failed to parse unified packet");
        return;
    }

    ESP_LOGI(TAG, "Large packet: msg_type=0x%02X, flags=0x%02X, seq=%u, payload_size=%d, total_size=%d",
             msg_type, flags, sequence_number, (int)payload_size, (int)packet_len);

    // Route to unified packet router
    s_packet_router.route_packet(packet_data, packet_len);
#else
    (void)packet_data;
    (void)packet_len;
#endif
}

// Handle control messages received FROM Daisy (frontend commands to backend)
static void handle_control_message_from_daisy(const uint8_t* packet_data, size_t packet_len)
{
#ifdef ESP_PLATFORM
    // Parse unified packet to extract message info
    uint8_t msg_type, flags;
    uint16_t sequence_number;
    uint8_t payload[MAX_PAYLOAD_SIZE];
    size_t payload_size;

    if (!parse_wave_packet(packet_data, packet_len, msg_type, payload, payload_size, sequence_number, flags)) {
        ESP_LOGE(TAG, "Failed to parse control message packet");
        return;
    }

    ESP_LOGI(TAG, "Received control message from Daisy: msg_type=0x%02X, flags=0x%02X, seq=%u, payload_size=%d",
             msg_type, flags, sequence_number, (int)payload_size);

    // Forward control messages to the inter_mcu layer for processing
    extern void inter_mcu_process_daisy_control_message(uint8_t type, const uint8_t* payload, uint8_t len);
    inter_mcu_process_daisy_control_message(msg_type, payload, payload_size);
    
    // Update packet statistics
    inter_mcu_increment_packet_stat(msg_type);
#else
    (void)packet_data;
    (void)packet_len;
#endif
}

// Handle new format control messages from Daisy (message type in payload)
static void handle_control_message_from_daisy_new_format(uint8_t msg_type, const uint8_t* payload, uint8_t len, uint8_t flags = 0, uint16_t sequence_number = 0)
{
#ifdef ESP_PLATFORM
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

// ============================================================================
// SPI Link Functions
// ============================================================================

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
        
        // If the last packet was one-way, don't send a response
        if (s_last_packet_was_one_way) {
            WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "Last packet was one-way, not sending response");
            s_last_packet_was_one_way = false; // Reset flag
            return;
        }
        
        WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "No messages to send, preparing no-data response");
        // Send a proper "no data" packet using unified format
        uint8_t msg_type = WaveX::Protocol::MSG_ACK;
        size_t packet_size = ProtocolHandler::CreateWaveXPacket(
            tx_buf, len, static_cast<WaveX::Protocol::MessageType>(msg_type), 
            nullptr, 0, 0, 0x80  // PKT_FLAG_ACK
        );
        
        if (packet_size > 0) {
            WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "Prepared no-data packet: size=%d", (int)packet_size);
        }
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
    
    // Prepare response packet using unified format
    size_t packet_size = ProtocolHandler::CreateWaveXPacket(
        tx_buf, len, static_cast<WaveX::Protocol::MessageType>(msg_type), 
        msg_payload, msg_len, entry->seq_num, 0
    );
    
    if (packet_size == 0) {
        ESP_LOGE(TAG, "Failed to create unified packet for type %d", msg_type);
        return;
    }
    
    WAVEX_LOG_ESP32_SPI(ESP32_INTER_SPI, "Prepared TX buffer: type=0x%02X, len=%d, msg_type=0x%02X, payload=%.*s", 
             msg_type & 0xFF, (int)packet_size, msg_type & 0xFF, (int)msg_len, msg_payload);
}

static void clear_transmitted_message_from_queue()
{
    // Enter critical section to protect queue operations
    taskENTER_CRITICAL(&s_spi_mutex);
    
    if (msg_queue_count == 0) {
        taskEXIT_CRITICAL(&s_spi_mutex);
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
    msg_queue_count = msg_queue_count - 1;
    
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

// ============================================================================
// Public API Functions
// ============================================================================

esp_err_t spi_link_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI link");
    
    // Initialize packet router
    init_packet_router();
    
    // Allocate DMA-capable buffers
    esp_err_t ret = allocate_dma_buffers();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffers: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure attention GPIO (output to Daisy)
    gpio_config_t attn_config = {};
    attn_config.pin_bit_mask = (1ULL << WAVEX_INTER_MCU_GPIO_ATTN);
    attn_config.mode = GPIO_MODE_OUTPUT;
    attn_config.pull_up_en = GPIO_PULLUP_DISABLE;
    attn_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    attn_config.intr_type = GPIO_INTR_DISABLE;
    
    ret = gpio_config(&attn_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure attention GPIO: %s", esp_err_to_name(ret));
        free_dma_buffers();
        return ret;
    }
    
    // Initialize attention signal to low (no urgent data)
    gpio_set_level((gpio_num_t)WAVEX_INTER_MCU_GPIO_ATTN, 0);
    
    ESP_LOGI(TAG, "SPI link initialized successfully");
    return ESP_OK;
}

esp_err_t spi_link_start(void)
{
    ESP_LOGI(TAG, "Starting SPI link");
    
    // Configure SPI slave
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = WAVEX_INTER_MCU_GPIO_MOSI;
    buscfg.miso_io_num = WAVEX_INTER_MCU_GPIO_MISO;
    buscfg.sclk_io_num = WAVEX_INTER_MCU_GPIO_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = MAX_PKT_SIZE;
    
    spi_slave_interface_config_t slavecfg = {};
    slavecfg.mode = 0;  // SPI mode 0 (CPOL=0, CPHA=0)
    slavecfg.spics_io_num = WAVEX_INTER_MCU_GPIO_CS;
    slavecfg.queue_size = 3;
    slavecfg.flags = 0;
    slavecfg.post_setup_cb = NULL;
    slavecfg.post_trans_cb = NULL;
    
    esp_err_t ret = spi_slave_initialize(WAVEX_INTER_MCU_SPI_HOST, &buscfg, &slavecfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI slave: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "SPI slave initialized successfully");
    ESP_LOGI(TAG, "SPI pins: SCLK=%d, MOSI=%d, MISO=%d, CS=%d", 
             WAVEX_INTER_MCU_GPIO_SCLK, WAVEX_INTER_MCU_GPIO_MOSI, 
             WAVEX_INTER_MCU_GPIO_MISO, WAVEX_INTER_MCU_GPIO_CS);
    
    // Create SPI slave task
    BaseType_t task_ret = xTaskCreate(
        spi_slave_task,
        "spi_slave",
        4096,  // Stack size
        NULL,  // Parameters
        5,     // Priority
        NULL   // Task handle
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SPI slave task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SPI slave task created successfully");
    return ESP_OK;
}

// SPI slave task - handles continuous communication with Daisy
static void spi_slave_task(void* pvParameters)
{
    ESP_LOGI(TAG, "SPI slave task started (triple buffering)");
    
    while (1) {
        // Get current buffer indices
        int tx_idx = s_current_tx_index;
        int rx_idx = s_current_rx_index;
        
        // Prepare TX buffer with queued message
        prepare_tx_buffer_without_consuming(s_tx_buffers[tx_idx], MAX_PKT_SIZE);
        
        // Set up transaction - use maximum packet size for SPI communication
        memset(&s_transactions[rx_idx], 0, sizeof(s_transactions[rx_idx]));
        s_transactions[rx_idx].length = MAX_PKT_SIZE * 8; // Maximum packet size for SPI
        s_transactions[rx_idx].tx_buffer = s_tx_buffers[tx_idx];
        s_transactions[rx_idx].rx_buffer = s_rx_buffers[rx_idx];
        
        // Clear RX buffer before transaction to prevent stale data
        memset(s_rx_buffers[rx_idx], 0, MAX_PKT_SIZE);
        
        // Wait for transaction from master
        esp_err_t ret = spi_slave_transmit(WAVEX_INTER_MCU_SPI_HOST, &s_transactions[rx_idx], portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI slave transaction failed: %s", esp_err_to_name(ret));
            continue;
        }
        
        // Process received data - determine actual packet size from first byte
        size_t rx_len = s_transactions[rx_idx].length / 8; // Convert bits to bytes
        if (rx_len > 0) {
            ESP_LOGD(TAG, "Received %d bytes from Daisy (buffer %d)", (int)rx_len, rx_idx);
            
            // Check for suspicious packet patterns that might indicate corruption
            uint8_t flags_size = s_rx_buffers[rx_idx][0];
            uint8_t msg_type = s_rx_buffers[rx_idx][1];
            if (flags_size == 0xA0 && msg_type == 0xA8) {
                ESP_LOGW(TAG, "DETECTED CORRUPTED PACKET PATTERN: flags_size=0x%02X, msg_type=0x%02X", flags_size, msg_type);
                ESP_LOGW(TAG, "This suggests SPI transmission corruption or buffer initialization issue");
            }
            
            // Debug: Log first few bytes of received packet
            ESP_LOGD(TAG, "RX packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                     s_rx_buffers[rx_idx][0], s_rx_buffers[rx_idx][1], s_rx_buffers[rx_idx][2], s_rx_buffers[rx_idx][3],
                     s_rx_buffers[rx_idx][4], s_rx_buffers[rx_idx][5], s_rx_buffers[rx_idx][6], s_rx_buffers[rx_idx][7]);
            
            // Determine actual packet size from first byte (size code)
            uint8_t size_code = s_rx_buffers[rx_idx][0] & PKT_SIZE_MASK;
            size_t expected_packet_size = ProtocolHandler::GetPacketSizeFromCode(size_code);
            
            if (expected_packet_size > 0 && expected_packet_size <= rx_len) {
                ESP_LOGD(TAG, "Expected packet size: %d bytes", (int)expected_packet_size);
                
                // Validate and process the packet with actual size
                if (validate_wave_packet(s_rx_buffers[rx_idx], expected_packet_size)) {
                    // Route to packet router for processing
                    s_packet_router.route_packet(s_rx_buffers[rx_idx], expected_packet_size);
                } else {
                    // Detailed packet analysis for debugging
                    uint8_t flags_size = s_rx_buffers[rx_idx][0];
                    uint8_t msg_type = s_rx_buffers[rx_idx][1];
                    uint16_t seq = s_rx_buffers[rx_idx][2] | (s_rx_buffers[rx_idx][3] << 8);
                    uint8_t flags = PKT_GET_FLAGS(flags_size);
                    uint8_t size_code = flags_size & PKT_SIZE_MASK;
                    
                    ESP_LOGW(TAG, "Invalid packet - size=%d, flags_size=0x%02X, msg_type=0x%02X, seq=%u, flags=0x%02X, size_code=%d", 
                             (int)expected_packet_size, flags_size, msg_type, seq, flags, size_code);
                    
                    // Log more packet bytes for analysis
                    ESP_LOGW(TAG, "Packet bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", 
                             s_rx_buffers[rx_idx][0], s_rx_buffers[rx_idx][1], s_rx_buffers[rx_idx][2], s_rx_buffers[rx_idx][3],
                             s_rx_buffers[rx_idx][4], s_rx_buffers[rx_idx][5], s_rx_buffers[rx_idx][6], s_rx_buffers[rx_idx][7],
                             s_rx_buffers[rx_idx][8], s_rx_buffers[rx_idx][9], s_rx_buffers[rx_idx][10], s_rx_buffers[rx_idx][11],
                             s_rx_buffers[rx_idx][12], s_rx_buffers[rx_idx][13], s_rx_buffers[rx_idx][14], s_rx_buffers[rx_idx][15]);
                    
                    // Log CRC bytes
                    ESP_LOGW(TAG, "CRC bytes: %02X %02X (last 2 bytes)", 
                             s_rx_buffers[rx_idx][expected_packet_size-2], s_rx_buffers[rx_idx][expected_packet_size-1]);
                }
            } else {
                ESP_LOGW(TAG, "Invalid packet size code: 0x%02X (expected size: %d, received: %d)", 
                         size_code, (int)expected_packet_size, (int)rx_len);
            }
        }
        
        // Clear transmitted message from queue after successful transmission
        clear_transmitted_message_from_queue();
        
        // Rotate buffer indices for next transaction
        s_current_tx_index = (s_current_tx_index + 1) % BUFFER_POOL_SIZE;
        s_current_rx_index = (s_current_rx_index + 1) % BUFFER_POOL_SIZE;
        
        ESP_LOGD(TAG, "Buffer rotation: TX=%d, RX=%d", s_current_tx_index, s_current_rx_index);
        
        // Small delay to prevent overwhelming the system
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int spi_link_send(uint16_t type, const void* payload, uint16_t len)
{
    if (!payload || len == 0) {
        return -1;
    }
    
    // Check if queue is full
    if (msg_queue_count >= MSG_QUEUE_SIZE) {
        ESP_LOGW(TAG, "Message queue full, dropping packet");
        return -1;
    }
    
    // Enter critical section to protect queue operations
    taskENTER_CRITICAL(&s_spi_mutex);
    
    // Copy message data to queue entry
    msg_queue_entry_t* entry = &msg_queue[msg_queue_tail];
    entry->type = type;
    entry->len = (len > sizeof(entry->payload)) ? sizeof(entry->payload) : len;
    entry->seq_num = next_seq_num++;
    entry->pending = true;
    
    if (len > 0 && len <= sizeof(entry->payload)) {
        memcpy(entry->payload, payload, len);
    }
    
    msg_queue_tail = (msg_queue_tail + 1) % MSG_QUEUE_SIZE;
    msg_queue_count = msg_queue_count + 1;
    
    bool was_empty = (msg_queue_count == 1);
    
    taskEXIT_CRITICAL(&s_spi_mutex);
    
    // Signal Daisy if this is the first message in queue
    if (was_empty) {
        signal_daisy_urgent(true);
        ESP_LOGI(TAG, "Signaled Daisy for urgent data - queue_count=%d", msg_queue_count);
    }
    
    ESP_LOGD(TAG, "Queued message: type=0x%02X, len=%d, seq=%d", type, len, entry->seq_num);
    return len;
}

int spi_link_recv(void** out)
{
    // This function is not implemented in the unified packet system
    // as we now use the packet router for incoming messages
    (void)out;
    return 0;
}

void spi_link_recycle(void* p, int is_rx)
{
    // This function is not implemented in the unified packet system
    (void)p;
    (void)is_rx;
}

void spi_link_get_stats(spi_link_stats_t* stats)
{
    if (!stats) return;
    
    // Initialize stats structure
    memset(stats, 0, sizeof(spi_link_stats_t));
    
    // Fill in basic stats
    stats->packets_sent = 0; // TODO: implement packet counting
    stats->packets_received = 0; // TODO: implement packet counting
    stats->crc_errors = 0; // TODO: implement error counting
    stats->irq_count = 0; // TODO: implement IRQ counting
    stats->rx_pool_empty = 0; // TODO: implement pool empty counting
    stats->last_activity_ms = 0; // TODO: implement activity tracking
}

void spi_link_log_stats(void)
{
    spi_link_stats_t stats;
    spi_link_get_stats(&stats);
    
    ESP_LOGI(TAG, "SPI Link Stats:");
    ESP_LOGI(TAG, "  Packets sent: %lu", (unsigned long)stats.packets_sent);
    ESP_LOGI(TAG, "  Packets received: %lu", (unsigned long)stats.packets_received);
    ESP_LOGI(TAG, "  CRC errors: %lu", (unsigned long)stats.crc_errors);
    ESP_LOGI(TAG, "  IRQ count: %lu", (unsigned long)stats.irq_count);
    ESP_LOGI(TAG, "  RX pool empty: %lu", (unsigned long)stats.rx_pool_empty);
    ESP_LOGI(TAG, "  Last activity: %lu ms", (unsigned long)stats.last_activity_ms);
}

bool spi_link_is_active(void)
{
    // Return true if the SPI link is active
    return true; // TODO: implement proper active state tracking
}

void spi_link_set_packet_callback(void (*callback)(const uint8_t* data, size_t length))
{
    // This function is not implemented in the unified packet system
    // as we now use the packet router for incoming messages
    (void)callback;
}

esp_err_t spi_link_stop(void)
{
    ESP_LOGI(TAG, "Stopping SPI link");
    
    // Free DMA buffers
    free_dma_buffers();
    
    ESP_LOGI(TAG, "SPI link stopped");
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
