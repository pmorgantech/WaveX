#include "inter_mcu.h"
#include "hardware_pins.h"
#include "spi_protocol/protocol.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "inter_mcu";

// UART configuration for inter-MCU communication
#define INTER_MCU_UART_NUM UART_NUM_1
#define INTER_MCU_UART_TX_PIN GPIO_NUM_17  // Correct inter-MCU UART TX pin
#define INTER_MCU_UART_RX_PIN GPIO_NUM_18  // Correct inter-MCU UART RX pin
#define INTER_MCU_UART_BAUD_RATE 460800   // Match Daisy baud rate
#define INTER_MCU_UART_BUFFER_SIZE 512

// Listeners
static wavex_meter_cb_t s_meter_cb = NULL;
static wavex_wave_chunk_cb_t s_wave_cb = NULL;
static void* s_meter_ud = NULL;
static void* s_wave_ud = NULL;
static TaskHandle_t s_rx_task_handle = nullptr;
static TaskHandle_t s_tx_task_handle = nullptr;
static volatile bool s_suspended = false;
static volatile bool s_tx_active = false;
static wavex_backend_heartbeat_t s_backend_hb = {};
static portMUX_TYPE s_hb_lock = portMUX_INITIALIZER_UNLOCKED;

// Message queue for outgoing messages
static QueueHandle_t s_tx_queue = nullptr;
#define TX_QUEUE_SIZE 16

// Packet statistics tracking
static wavex_packet_stats_t s_packet_stats = {};
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

// TX statistics tracking
static wavex_tx_stats_t s_tx_stats = {};
static portMUX_TYPE s_tx_stats_lock = portMUX_INITIALIZER_UNLOCKED;

// Helper function to increment packet statistics
static void increment_packet_stat(uint8_t packet_type) {
    taskENTER_CRITICAL(&s_stats_lock);
    s_packet_stats.total_packets++;
    
    switch (packet_type) {
        case WaveX::Protocol::MSG_SYNC: s_packet_stats.sync_packets++; break;
        case WaveX::Protocol::MSG_CONTROL_CHANGE: s_packet_stats.control_change_packets++; break;
        case WaveX::Protocol::MSG_NOTE_ON: s_packet_stats.note_on_packets++; break;
        case WaveX::Protocol::MSG_NOTE_OFF: s_packet_stats.note_off_packets++; break;
        case WaveX::Protocol::MSG_SAMPLE_LOAD: s_packet_stats.sample_load_packets++; break;
        case WaveX::Protocol::MSG_SAMPLE_DATA: s_packet_stats.sample_data_packets++; break;
        case WaveX::Protocol::MSG_PARAMETER_UPDATE: s_packet_stats.parameter_update_packets++; break;
        case WaveX::Protocol::MSG_STATUS_REQUEST: s_packet_stats.status_request_packets++; break;
        case WaveX::Protocol::MSG_STATUS_RESPONSE: s_packet_stats.status_response_packets++; break;
        case WaveX::Protocol::MSG_SAMPLE_CTRL: s_packet_stats.sample_ctrl_packets++; break;
        case WaveX::Protocol::MSG_PREVIEW_REQ: s_packet_stats.preview_req_packets++; break;
        case WaveX::Protocol::MSG_DATA_REQUEST: s_packet_stats.data_request_packets++; break;
        case WaveX::Protocol::MSG_METER_PUSH: s_packet_stats.meter_push_packets++; break;
        case WaveX::Protocol::MSG_WAVE_CHUNK: s_packet_stats.wave_chunk_packets++; break;
        case WaveX::Protocol::MSG_HEARTBEAT: s_packet_stats.heartbeat_packets++; break;
        case WaveX::Protocol::MSG_ERROR: s_packet_stats.error_packets++; break;
        default: s_packet_stats.unknown_packets++; break;
    }
    taskEXIT_CRITICAL(&s_stats_lock);
}

// RX task: continuously receives and parses incoming UART messages
static void inter_mcu_rx_task(void* arg) {
    static uint8_t rxbuf[INTER_MCU_UART_BUFFER_SIZE];
    static uint8_t frame_buffer[INTER_MCU_UART_BUFFER_SIZE];
    static size_t frame_pos = 0;
    static bool frame_in_progress = false;
    static uint32_t last_frame_time = 0;
    static size_t expected_total = 0; // total bytes expected for ProtocolHandler frame
    static bool s_seen_any_rx = false;
    static uint32_t s_total_rx_bytes = 0;
static uint32_t s_total_raw_bytes = 0;
    // Note: inversion is configured statically during init; no auto-toggling here
    
    ESP_LOGI(TAG, "UART RX task started");
    // Quick RX pin status check
    {
        int level = gpio_get_level(INTER_MCU_UART_RX_PIN);
        ESP_LOGI(TAG, "RX pin initial level: %s", level ? "HIGH" : "LOW");
    }
    
    // Periodic status logging
    uint32_t last_status_log = 0;
    uint32_t no_data_cycles = 0;
    
    while (true) {
        if (s_suspended) { 
            ESP_LOGD(TAG, "RX task suspended, waiting...");
            vTaskDelay(pdMS_TO_TICKS(10)); 
            continue; 
        }
        
        // Read data from UART
        int len = uart_read_bytes(INTER_MCU_UART_NUM, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(20));
        if (len > 0) {
            no_data_cycles = 0;  // Reset no-data counter
            s_total_raw_bytes += (uint32_t)len;
            if (!s_seen_any_rx) {
                s_seen_any_rx = true;
                ESP_LOGI(TAG, "First RX: %d bytes, first=0x%02X", len, rxbuf[0]);
            }
            // Robust sync recovery: Search for sync byte in entire data stream
            for (int i = 0; i < len; i++) {
                uint8_t byte = rxbuf[i];
                
                if (!frame_in_progress) {
                    // Not in a frame - look for sync byte anywhere in the stream
                    if (byte == WaveX::Protocol::SYNC_BYTE) {
                        frame_in_progress = true;
                        frame_pos = 0;
                        expected_total = 0;
                        frame_buffer[frame_pos++] = byte; // SYNC
                        last_frame_time = esp_timer_get_time();
                    } else {
                        // Not a sync byte - continue searching
                    }
                } else {
                    frame_buffer[frame_pos++] = byte;
                    // Once we have 4 bytes (ProtocolHeader), compute expected total
                    if (expected_total == 0 && frame_pos >= 4) {
                        uint8_t type = frame_buffer[1];
                        uint8_t length = frame_buffer[2];
                        (void)type;
                        expected_total = 4 + length; // header + payload
                        if (expected_total > sizeof(frame_buffer)) {
                            // Invalid, reset
                            ESP_LOGW(TAG, "Frame too large: %d > %d, resetting", expected_total, (int)sizeof(frame_buffer));
                            frame_in_progress = false;
                            frame_pos = 0;
                            continue;
                        }
                    }
                    // When full frame is available, validate and process
                    if (expected_total != 0 && frame_pos >= expected_total) {
                        uint8_t type = frame_buffer[1];
                        uint8_t length = frame_buffer[2];
                        uint8_t checksum = frame_buffer[3];
                        uint8_t calc = WaveX::Protocol::ProtocolHandler::CalculateChecksum(&frame_buffer[4], length);
                        if (calc == checksum) {
                            s_total_rx_bytes += expected_total; // Count valid frames
                            using namespace WaveX::Protocol;
                            const uint8_t* payload = &frame_buffer[4];
                            
                            // Increment packet statistics
                            increment_packet_stat(type);
                            
                            // Log packet type with descriptive message (throttled for frequent packets)
                            const char* packet_type_name = "UNKNOWN";
                            bool should_log = true;
                            
                            switch (type) {
                                case MSG_SYNC: packet_type_name = "SYNC"; break;
                                case MSG_CONTROL_CHANGE: packet_type_name = "CONTROL_CHANGE"; break;
                                case MSG_NOTE_ON: packet_type_name = "NOTE_ON"; break;
                                case MSG_NOTE_OFF: packet_type_name = "NOTE_OFF"; break;
                                case MSG_SAMPLE_LOAD: packet_type_name = "SAMPLE_LOAD"; break;
                                case MSG_SAMPLE_DATA: packet_type_name = "SAMPLE_DATA"; break;
                                case MSG_PARAMETER_UPDATE: packet_type_name = "PARAMETER_UPDATE"; break;
                                case MSG_STATUS_REQUEST: packet_type_name = "STATUS_REQUEST"; break;
                                case MSG_STATUS_RESPONSE: packet_type_name = "STATUS_RESPONSE"; break;
                                case MSG_SAMPLE_CTRL: packet_type_name = "SAMPLE_CTRL"; break;
                                case MSG_PREVIEW_REQ: packet_type_name = "PREVIEW_REQ"; break;
                                case MSG_DATA_REQUEST: packet_type_name = "DATA_REQUEST"; break;
                                case MSG_METER_PUSH: 
                                    packet_type_name = "METER_PUSH"; 
                                    // Only log every 100th METER_PUSH packet
                                    should_log = (s_packet_stats.meter_push_packets % 100 == 0);
                                    break;
                                case MSG_WAVE_CHUNK: packet_type_name = "WAVE_CHUNK"; break;
                                case MSG_HEARTBEAT: packet_type_name = "HEARTBEAT"; break;
                                case MSG_ERROR: packet_type_name = "ERROR"; break;
                                default: packet_type_name = "UNKNOWN"; break;
                            }
                            
                            if (should_log) {
                                ESP_LOGI(TAG, "Received %s packet: type=0x%02X, len=%d, checksum=0x%02X ✓", 
                                        packet_type_name, type, length, checksum);
                            }
                            
                            switch (type) {
                                case MSG_METER_PUSH: {
                                    if (length == sizeof(MeterPushMessage)) {
                                        MeterPushMessage m{};
                                        memcpy(&m, payload, sizeof(m));
                                        if (s_meter_cb) {
                                            s_meter_cb(m.rms, m.peak, s_meter_ud);
                                        }
                                    }
                                    break;
                                }
                                case MSG_WAVE_CHUNK: {
                                    if (s_wave_cb && length >= sizeof(WaveChunkMessage)) {
                                        WaveChunkMessage h{};
                                        memcpy(&h, payload, sizeof(h));
                                        const uint16_t count = h.count;
                                        if (sizeof(WaveChunkMessage) + count * sizeof(int16_t) == length) {
                                            const int16_t* samples = reinterpret_cast<const int16_t*>(payload + sizeof(WaveChunkMessage));
                                            s_wave_cb(h.offset, samples, count, s_wave_ud);
                                        }
                                    }
                                    break;
                                }
                                case MSG_HEARTBEAT: {
                                    if (length == sizeof(HeartbeatMessage)) {
                                        HeartbeatMessage hb{};
                                        memcpy(&hb, payload, sizeof(hb));
                                        taskENTER_CRITICAL(&s_hb_lock);
                                        s_backend_hb.uptime_ms = hb.uptime_ms;
                                        s_backend_hb.rx_total = hb.rx_total;
                                        s_backend_hb.loop_counter = hb.loop_counter;
                                        s_backend_hb.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
                                        s_backend_hb.valid = true;
                                        taskEXIT_CRITICAL(&s_hb_lock);
                                    }
                                    break;
                                }
                                case MSG_SYNC: {
                                    break;
                                }
                                default:
                                    ESP_LOGW(TAG, "Unknown msg type: 0x%02x", type);
                                    break;
                            }
                        } else {
                            // Increment invalid packet counter
                            taskENTER_CRITICAL(&s_stats_lock);
                            s_packet_stats.invalid_packets++;
                            taskEXIT_CRITICAL(&s_stats_lock);
                            
                            ESP_LOGW(TAG, "INVALID FRAME: checksum mismatch, searching for next sync...");
                            // Reset frame state and continue searching for sync
                            frame_in_progress = false;
                            frame_pos = 0;
                            expected_total = 0;
                            // Continue processing remaining bytes for potential sync
                        }
                        // reset for next frame (valid path)
                        frame_in_progress = false;
                        frame_pos = 0;
                        expected_total = 0;
                    }
                    // Timeout check - if frame is taking too long, abandon and resync
                    uint32_t now = esp_timer_get_time();
                    if (frame_in_progress && (now - last_frame_time) > 50000) { // 50ms timeout
                        ESP_LOGD(TAG, "Frame timeout after %d bytes, resyncing", frame_pos);
                        frame_in_progress = false;
                        frame_pos = 0;
                        expected_total = 0;
                    }
                }
            }
        } else {
            // No data received - track this for debugging
            no_data_cycles++;
        }
        
        // Periodic status logging (every 10 seconds)
        uint32_t now = xTaskGetTickCount();
        if (now - last_status_log > pdMS_TO_TICKS(10000)) {
            // Check UART buffer status
            size_t buffered = 0;
            uart_get_buffered_data_len(INTER_MCU_UART_NUM, &buffered);
            
            // Get current packet statistics
            wavex_packet_stats_t current_stats;
            taskENTER_CRITICAL(&s_stats_lock);
            current_stats = s_packet_stats;
            taskEXIT_CRITICAL(&s_stats_lock);
            
            ESP_LOGI(TAG, "RX Status: raw_bytes=%lu, parsed_frames=%lu, buffered=%u", 
                    (unsigned long)s_total_raw_bytes,
                    (unsigned long)s_total_rx_bytes,
                    (unsigned)buffered);
            ESP_LOGI(TAG, "Packet Stats: total=%lu, valid=%lu, invalid=%lu", 
                    (unsigned long)current_stats.total_packets,
                    (unsigned long)(current_stats.total_packets - current_stats.invalid_packets),
                    (unsigned long)current_stats.invalid_packets);
            
            // Log packet type breakdown (only if we have packets)
            if (current_stats.total_packets > 0) {
                ESP_LOGI(TAG, "Packet Types: METER=%lu, HEARTBEAT=%lu, SYNC=%lu, WAVE=%lu, CTRL=%lu", 
                        (unsigned long)current_stats.meter_push_packets,
                        (unsigned long)current_stats.heartbeat_packets,
                        (unsigned long)current_stats.sync_packets,
                        (unsigned long)current_stats.wave_chunk_packets,
                        (unsigned long)(current_stats.control_change_packets + current_stats.note_on_packets + 
                                      current_stats.note_off_packets + current_stats.sample_ctrl_packets));
            }
            
            // Get and log TX statistics
            wavex_tx_stats_t tx_stats;
            inter_mcu_get_tx_stats(&tx_stats);
            ESP_LOGI(TAG, "TX Status: total_sent=%lu, pings=%lu, tests=%lu, last_send=%lu", 
                    (unsigned long)tx_stats.total_messages_sent,
                    (unsigned long)tx_stats.ping_messages_sent,
                    (unsigned long)tx_stats.test_messages_sent,
                    (unsigned long)tx_stats.last_send_time);
            
            // Debug: warn if still no parsed frames after some time and trigger sync recovery
            static uint32_t last_sync_recovery = 0;
            if (s_total_rx_bytes == 0 && s_total_raw_bytes > 1000 && 
                (now - last_sync_recovery) > pdMS_TO_TICKS(15000)) {
                ESP_LOGW(TAG, "No valid frames parsed despite receiving %lu raw bytes - triggering sync recovery", 
                        (unsigned long)s_total_raw_bytes);
                uart_flush_input(INTER_MCU_UART_NUM);
                last_sync_recovery = now;
            }
            
            last_status_log = now;
            no_data_cycles = 0;  // Reset for next period
        }
        
        // Small delay to prevent busy waiting
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// TX task: sends queued messages via UART
static void inter_mcu_tx_task(void* arg) {
    static uint8_t txbuf[INTER_MCU_UART_BUFFER_SIZE];
    static uint32_t last_test_time = 0;
    
    ESP_LOGI(TAG, "UART TX task started");
    
    while (true) {
        // Send ping message every 5 seconds to keep communication active
        uint32_t now = xTaskGetTickCount();
        if (now - last_test_time > pdMS_TO_TICKS(5000)) {
            // Simple ping message
            uint8_t ping_packet[64];
            size_t ping_len = WaveX::Protocol::ProtocolHandler::CreateGenericPacket(
                ping_packet, sizeof(ping_packet),
                WaveX::Protocol::MSG_SYNC,
                nullptr, 0
            );
            
            if (ping_len > 0) {
                uart_write_bytes(INTER_MCU_UART_NUM, ping_packet, ping_len);
                ESP_LOGD(TAG, "Sent ping packet (%d bytes)", ping_len);
                
                // Update TX statistics
                taskENTER_CRITICAL(&s_tx_stats_lock);
                s_tx_stats.ping_messages_sent++;
                s_tx_stats.total_messages_sent++;
                s_tx_stats.last_send_time = now;
                taskEXIT_CRITICAL(&s_tx_stats_lock);
            }
            last_test_time = now;
        }
        
        // Send comprehensive test messages every 30 seconds to verify UART and protocol are working
        static uint32_t last_comprehensive_test = 0;
        if (now - last_comprehensive_test > pdMS_TO_TICKS(30000)) {
            // Test 1: SYNC message
            uint8_t sync_packet[64];
            size_t sync_len = WaveX::Protocol::ProtocolHandler::CreateGenericPacket(
                sync_packet, sizeof(sync_packet), 
                WaveX::Protocol::MSG_SYNC, 
                nullptr, 0
            );
            
            if (sync_len > 0) {
                uart_write_bytes(INTER_MCU_UART_NUM, sync_packet, sync_len);
                ESP_LOGD(TAG, "Sent SYNC test packet (%d bytes)", sync_len);
            }
            
            // Test 2: Control change message (test parameter update)
            uint8_t ctrl_packet[64];
            size_t ctrl_len = WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(
                ctrl_packet, sizeof(ctrl_packet),
                0x01,  // parameter: some test parameter
                0x00,  // channel: 0
                0x7F   // value: 127 (mid-range)
            );
            
            if (ctrl_len > 0) {
                uart_write_bytes(INTER_MCU_UART_NUM, ctrl_packet, ctrl_len);
                ESP_LOGD(TAG, "Sent CONTROL_CHANGE test packet (%d bytes)", ctrl_len);
            }
            
            // Test 3: Note on message (test MIDI functionality)
            uint8_t note_packet[64];
            size_t note_len = WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(
                note_packet, sizeof(note_packet),
                0x3C,  // note: middle C
                0x40,  // velocity: 64 (medium)
                0x00   // channel: 0
            );
            
            if (note_len > 0) {
                uart_write_bytes(INTER_MCU_UART_NUM, note_packet, note_len);
                ESP_LOGD(TAG, "Sent NOTE_ON test packet (%d bytes)", note_len);
            }
            
            // Test 4: Status request (test query functionality)
            uint8_t status_packet[64];
            size_t status_len = WaveX::Protocol::ProtocolHandler::CreateGenericPacket(
                status_packet, sizeof(status_packet),
                WaveX::Protocol::MSG_STATUS_REQUEST,
                nullptr, 0
            );
            
            if (status_len > 0) {
                uart_write_bytes(INTER_MCU_UART_NUM, status_packet, status_len);
                ESP_LOGD(TAG, "Sent STATUS_REQUEST test packet (%d bytes)", status_len);
            }
            
            ESP_LOGI(TAG, "Sent 4 test packets to Daisy (SYNC, CONTROL, NOTE, STATUS)");
            
            // Update TX statistics
            taskENTER_CRITICAL(&s_tx_stats_lock);
            s_tx_stats.test_messages_sent++;
            s_tx_stats.total_messages_sent += 4; // We sent 4 packets
            s_tx_stats.last_send_time = now;
            taskEXIT_CRITICAL(&s_tx_stats_lock);
            
            last_test_time = now;
        }
        
        // Wait for message in queue
        if (xQueueReceive(s_tx_queue, txbuf, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Extract message length from first byte (length is stored in first byte of queue item)
            size_t msg_len = txbuf[0];
            const uint8_t* msg_data = &txbuf[1];
            
            if (msg_len > 0 && msg_len <= INTER_MCU_UART_BUFFER_SIZE - 1) {
                s_tx_active = true;
                
                // Send data via UART
                int written = uart_write_bytes(INTER_MCU_UART_NUM, msg_data, msg_len);
                if (written == msg_len) {
                    ESP_LOGD(TAG, "Sent message: %d bytes", written);
                } else {
                    ESP_LOGW(TAG, "UART write failed: expected %d, wrote %d", msg_len, written);
                }
                
                s_tx_active = false;
            }
        }
    }
}

// Initialize UART for inter-MCU communication
esp_err_t inter_mcu_init(void) {
    ESP_LOGI(TAG, "inter_mcu_init(): UART initialization");
    
    // Configure UART
    uart_config_t uart_config = {};
    uart_config.baud_rate = INTER_MCU_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_param_config(INTER_MCU_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %d", ret);
        return ret;
    }

    ret = uart_set_pin(INTER_MCU_UART_NUM, INTER_MCU_UART_TX_PIN, INTER_MCU_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %d", ret);
        return ret;
    }

    ret = uart_driver_install(INTER_MCU_UART_NUM, INTER_MCU_UART_BUFFER_SIZE, INTER_MCU_UART_BUFFER_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %d", ret);
        return ret;
    }

    // Flush any pending noise and set a small RX timeout to wake reads
    uart_flush_input(INTER_MCU_UART_NUM);
    uart_set_rx_timeout(INTER_MCU_UART_NUM, 2); // ~2 char times
    // Start with RX inversion OFF to test signal polarity
    uart_set_line_inverse(INTER_MCU_UART_NUM, 0);
    ESP_LOGW(TAG, "UART RX line inversion DISABLED (testing non-inverted first)");
    
    // Configure GPIO to avoid biasing RX; Daisy drives actively
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << INTER_MCU_UART_RX_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    //gpio_config(&io_conf); //DEBUG

    // Diagnostic: check driver status and initial buffered data
    size_t buffered = 0;
    uart_get_buffered_data_len(INTER_MCU_UART_NUM, &buffered);
    int installed = uart_is_driver_installed(INTER_MCU_UART_NUM);
    ESP_LOGI(TAG, "UART driver status: installed=%d buffered=%u", installed, (unsigned)buffered);
    
    // Note: ESP32 UART doesn't expose status register directly
    ESP_LOGI(TAG, "UART configuration: %d baud, 8N1, no flow control", INTER_MCU_UART_BAUD_RATE);
    
    // Flush any existing data to start clean and avoid mid-transmission sync issues
    uart_flush_input(INTER_MCU_UART_NUM);
    ESP_LOGI(TAG, "UART input buffer flushed to ensure clean sync start");
    
    // Quick initial RX check
    uint8_t probe_buf[64];
    int len = uart_read_bytes(INTER_MCU_UART_NUM, probe_buf, sizeof(probe_buf), pdMS_TO_TICKS(100));
    if (len > 0) {
        ESP_LOGI(TAG, "Initial RX: %d bytes, first=0x%02X", len, probe_buf[0]);
    }

    uint32_t br = 0; uart_get_baudrate(INTER_MCU_UART_NUM, &br);
    ESP_LOGI(TAG, "UART initialized successfully (UART1 @ %lu baud, TX=GPIO%d RX=GPIO%d)", (unsigned long)br, INTER_MCU_UART_TX_PIN, INTER_MCU_UART_RX_PIN);
    return ESP_OK;
}

esp_err_t inter_mcu_start(void) {
    esp_err_t ret;
    
    // Create TX queue
    s_tx_queue = xQueueCreate(TX_QUEUE_SIZE, INTER_MCU_UART_BUFFER_SIZE);
    if (s_tx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return ESP_FAIL;
    }
    
    // Create RX task
    if (s_rx_task_handle == NULL) {
        xTaskCreatePinnedToCore(inter_mcu_rx_task, "inter_mcu_rx", 4096, NULL, 3, &s_rx_task_handle, 1);
        if (s_rx_task_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create RX task");
            return ESP_FAIL;
        }
    }
    
    // Create TX task
    if (s_tx_task_handle == NULL) {
        xTaskCreatePinnedToCore(inter_mcu_tx_task, "inter_mcu_tx", 4096, NULL, 3, &s_tx_task_handle, 1);
        if (s_tx_task_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create TX task");
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "Inter-MCU UART communication started successfully");
    ESP_LOGI(TAG, "UART1: TX=GPIO%d, RX=GPIO%d, Baud=%d", INTER_MCU_UART_TX_PIN, INTER_MCU_UART_RX_PIN, INTER_MCU_UART_BAUD_RATE);
    
    return ESP_OK;
}

// Helper function to queue a message for transmission
static esp_err_t queue_message(const uint8_t* data, size_t length) {
    if (s_tx_queue == NULL || data == NULL || length == 0) {
        return ESP_FAIL;
    }
    
    // Prepare queue item: first byte is length, followed by data
    uint8_t queue_item[INTER_MCU_UART_BUFFER_SIZE];
    queue_item[0] = (uint8_t)length;
    memcpy(&queue_item[1], data, length);
    
    if (xQueueSend(s_tx_queue, queue_item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, message dropped");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

void inter_mcu_get_backend_heartbeat(wavex_backend_heartbeat_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&s_hb_lock);
    *out = s_backend_hb;
    taskEXIT_CRITICAL(&s_hb_lock);
}

void inter_mcu_get_packet_stats(wavex_packet_stats_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&s_stats_lock);
    *out = s_packet_stats;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void inter_mcu_reset_packet_stats(void) {
    taskENTER_CRITICAL(&s_stats_lock);
    memset(&s_packet_stats, 0, sizeof(s_packet_stats));
    taskEXIT_CRITICAL(&s_stats_lock);
}

void inter_mcu_get_packet_summary(wavex_packet_summary_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&s_stats_lock);
    out->total_packets = s_packet_stats.total_packets;
    out->meter_packets = s_packet_stats.meter_push_packets;
    out->heartbeat_packets = s_packet_stats.heartbeat_packets;
    out->control_packets = s_packet_stats.control_change_packets + 
                           s_packet_stats.note_on_packets + 
                           s_packet_stats.note_off_packets + 
                           s_packet_stats.sample_ctrl_packets;
    out->invalid_packets = s_packet_stats.invalid_packets;
    taskEXIT_CRITICAL(&s_stats_lock);
}

uint32_t inter_mcu_get_meter_packet_count(void) {
    uint32_t count;
    taskENTER_CRITICAL(&s_stats_lock);
    count = s_packet_stats.meter_push_packets;
    taskEXIT_CRITICAL(&s_stats_lock);
    return count;
}

uint32_t inter_mcu_get_total_packet_count(void) {
    uint32_t count;
    taskENTER_CRITICAL(&s_stats_lock);
    count = s_packet_stats.total_packets;
    taskEXIT_CRITICAL(&s_stats_lock);
    return count;
}

int inter_mcu_format_packet_stats(char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return 0;
    
    taskENTER_CRITICAL(&s_stats_lock);
    wavex_packet_stats_t stats = s_packet_stats;
    taskEXIT_CRITICAL(&s_stats_lock);
    
    return snprintf(buffer, buffer_size,
        "Packets: Total=%lu, Valid=%lu, Invalid=%lu | "
        "METER=%lu, HEARTBEAT=%lu, SYNC=%lu, WAVE=%lu, CTRL=%lu",
        (unsigned long)stats.total_packets,
        (unsigned long)(stats.total_packets - stats.invalid_packets),
        (unsigned long)stats.invalid_packets,
        (unsigned long)stats.meter_push_packets,
        (unsigned long)stats.heartbeat_packets,
        (unsigned long)stats.sync_packets,
        (unsigned long)stats.wave_chunk_packets,
        (unsigned long)(stats.control_change_packets + stats.note_on_packets + 
                      stats.note_off_packets + stats.sample_ctrl_packets));
}

void inter_mcu_send_test_messages(void) {
    ESP_LOGI(TAG, "Manually sending test messages to Daisy...");
    
    // Send a variety of test messages
    uint8_t packet[64];
    
    // 1. SYNC message
    size_t len = WaveX::Protocol::ProtocolHandler::CreateGenericPacket(
        packet, sizeof(packet), WaveX::Protocol::MSG_SYNC, nullptr, 0);
    if (len > 0) {
        uart_write_bytes(INTER_MCU_UART_NUM, packet, len);
        ESP_LOGI(TAG, "Sent manual SYNC test packet (%d bytes)", len);
    }
    
    // 2. Control change message
    len = WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(
        packet, sizeof(packet), 0x02, 0x00, 0x64);
    if (len > 0) {
        uart_write_bytes(INTER_MCU_UART_NUM, packet, len);
        ESP_LOGI(TAG, "Sent manual CONTROL_CHANGE test packet (%d bytes)", len);
    }
    
    // 3. Note on message
    len = WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(
        packet, sizeof(packet), 0x40, 0x60, 0x00);
    if (len > 0) {
        uart_write_bytes(INTER_MCU_UART_NUM, packet, len);
        ESP_LOGI(TAG, "Sent manual NOTE_ON test packet (%d bytes)", len);
    }
    
    // 4. Status request
    len = WaveX::Protocol::ProtocolHandler::CreateGenericPacket(
        packet, sizeof(packet), WaveX::Protocol::MSG_STATUS_REQUEST, nullptr, 0);
    if (len > 0) {
        uart_write_bytes(INTER_MCU_UART_NUM, packet, len);
        ESP_LOGI(TAG, "Sent manual STATUS_REQUEST test packet (%d bytes)", len);
    }
    
    ESP_LOGI(TAG, "Manual test messages sent to Daisy");
    
    // Update TX statistics
    taskENTER_CRITICAL(&s_tx_stats_lock);
    s_tx_stats.test_messages_sent++;
    s_tx_stats.total_messages_sent += 4; // We sent 4 packets
    s_tx_stats.last_send_time = xTaskGetTickCount();
    taskEXIT_CRITICAL(&s_tx_stats_lock);
}

void inter_mcu_get_tx_stats(wavex_tx_stats_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&s_tx_stats_lock);
    *out = s_tx_stats;
    taskEXIT_CRITICAL(&s_tx_stats_lock);
}

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(buffer, sizeof(buffer), parameter, channel, value);
    if (len == 0) return ESP_FAIL;
    
    return queue_message(buffer, len);
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(buffer, sizeof(buffer), note, velocity, channel);
    if (len == 0) return ESP_FAIL;
    
    return queue_message(buffer, len);
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateNoteOffPacket(buffer, sizeof(buffer), note, channel);
    if (len == 0) return ESP_FAIL;
    
    return queue_message(buffer, len);
} 

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) {
    uint8_t buffer[128];
    WaveX::Protocol::SampleCtrlMessage m{};
    m.slot = slot;
    m.cmd = static_cast<uint8_t>(cmd);
    m.rate = rate;
    size_t len = WaveX::Protocol::ProtocolHandler::CreateSampleCtrlPacket(buffer, sizeof(buffer), m);
    if (len == 0) return ESP_FAIL;
    
    return queue_message(buffer, len);
}

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) {
    uint8_t buffer[128];
    WaveX::Protocol::PreviewReqMessage m{};
    m.slot = slot;
    m.start = start;
    m.end = end;
    m.decim = decim;
    size_t len = WaveX::Protocol::ProtocolHandler::CreatePreviewReqPacket(buffer, sizeof(buffer), m);
    if (len == 0) return ESP_FAIL;
    
    return queue_message(buffer, len);
}

void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data) {
    s_meter_cb = cb; s_meter_ud = user_data;
}

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) {
    s_wave_cb = cb; s_wave_ud = user_data;
}

extern "C" void inter_mcu_set_suspended(bool suspended) {
    s_suspended = suspended;
}

extern "C" bool inter_mcu_is_busy(void) {
    return s_tx_active;
}

extern "C" void inter_mcu_toggle_inversion(void) {
    // Toggle RX line inversion for debugging
    static bool inverted = false;  // Start non-inverted (current state)
    inverted = !inverted;
    
    if (inverted) {
        uart_set_line_inverse(INTER_MCU_UART_NUM, UART_SIGNAL_RXD_INV);
        ESP_LOGW(TAG, "RX line inversion ENABLED");
    } else {
        uart_set_line_inverse(INTER_MCU_UART_NUM, 0);
        ESP_LOGW(TAG, "RX line inversion DISABLED");
    }
    
    // Flush any pending data after inversion change
    uart_flush_input(INTER_MCU_UART_NUM);
}

#else // !ESP_PLATFORM (host/lint build stubs)

// Minimal stubs for non-ESP builds so linters and host builds pass
static wavex_meter_cb_t s_meter_cb = NULL;
static wavex_wave_chunk_cb_t s_wave_cb = NULL;
static void* s_meter_ud = NULL;
static void* s_wave_ud = NULL;

esp_err_t inter_mcu_init(void) { return ESP_OK; }
esp_err_t inter_mcu_start(void) { return ESP_OK; }
esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) { (void)parameter; (void)channel; (void)value; return ESP_OK; }
esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) { (void)note; (void)velocity; (void)channel; return ESP_OK; }
esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) { (void)note; (void)channel; return ESP_OK; }
esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) { (void)slot; (void)cmd; (void)rate; return ESP_OK; }
esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) { (void)slot; (void)start; (void)end; (void)decim; return ESP_OK; }
void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data) { s_meter_cb = cb; s_meter_ud = user_data; }
void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) { s_wave_cb = cb; s_wave_ud = user_data; }
extern "C" void inter_mcu_set_suspended(bool suspended) { (void)suspended; }
extern "C" bool inter_mcu_is_busy(void) { return false; }
extern "C" void inter_mcu_toggle_inversion(void) {}

#endif // ESP_PLATFORM