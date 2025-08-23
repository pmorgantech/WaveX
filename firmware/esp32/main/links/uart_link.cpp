#include "uart_link.h"
#include "../comm/statistics.h"
#include "../../shared/spi_protocol/protocol.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
static const char* TAG = "UartLink";
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[%s] DEBUG: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "UartLink";
#endif

UartLink::UartLink()
    : m_initialized(false)
    , m_started(false)
    , m_suspended(false)
    , m_tx_active(false)
    , m_rx_task_handle(NULL)
    , m_tx_task_handle(NULL)
    , m_tx_queue(NULL)
{
}

UartLink::~UartLink()
{
    stop_tasks();
    
    if (m_tx_queue) {
        #ifdef ESP_PLATFORM
        vQueueDelete(m_tx_queue);
        #endif
        m_tx_queue = NULL;
    }
}

esp_err_t UartLink::init()
{
    if (m_initialized) {
        ESP_LOGI(TAG, "UART link already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing UART link...");
    
    esp_err_t ret = configure_uart();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART configuration failed");
        return ret;
    }
    
    m_initialized = true;
    ESP_LOGI(TAG, "UART link initialized successfully");
    
    return ESP_OK;
}

esp_err_t UartLink::start()
{
    if (!m_initialized) {
        ESP_LOGE(TAG, "UART link not initialized");
        return -1; // ESP_ERR_INVALID_STATE
    }
    
    if (m_started) {
        ESP_LOGI(TAG, "UART link already started");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting UART link...");
    
    esp_err_t ret = create_tasks();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create tasks");
        return ret;
    }
    
    m_started = true;
    ESP_LOGI(TAG, "UART link started successfully");
    
    return ESP_OK;
}

esp_err_t UartLink::send_control_change(uint8_t parameter, uint8_t channel, uint16_t value)
{
    uint8_t buffer[128];
    size_t len = create_control_change_packet(buffer, sizeof(buffer), parameter, channel, value);
    if (len == 0) return -1; // ESP_FAIL
    
    return queue_message(buffer, len);
}

esp_err_t UartLink::send_note_on(uint8_t note, uint8_t velocity, uint8_t channel)
{
    uint8_t buffer[128];
    size_t len = create_note_on_packet(buffer, sizeof(buffer), note, velocity, channel);
    if (len == 0) return -1; // ESP_FAIL
    
    return queue_message(buffer, len);
}

esp_err_t UartLink::send_note_off(uint8_t note, uint8_t channel)
{
    uint8_t buffer[128];
    size_t len = create_note_off_packet(buffer, sizeof(buffer), note, channel);
    if (len == 0) return -1; // ESP_FAIL
    
    return queue_message(buffer, len);
}

esp_err_t UartLink::send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate)
{
    uint8_t buffer[128];
    size_t len = create_sample_ctrl_packet(buffer, sizeof(buffer), slot, cmd, rate);
    if (len == 0) return -1; // ESP_FAIL
    
    return queue_message(buffer, len);
}

esp_err_t UartLink::send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim)
{
    uint8_t buffer[128];
    size_t len = create_preview_req_packet(buffer, sizeof(buffer), slot, start, end, decim);
    if (len == 0) return -1; // ESP_FAIL
    
    return queue_message(buffer, len);
}

void UartLink::send_test_messages()
{
    ESP_LOGI(TAG, "Sending test messages...");
    
    // Send a variety of test messages
    uint8_t packet[64];
    
    // 1. SYNC message
    size_t len = create_generic_packet(packet, sizeof(packet), WaveX::Protocol::MSG_SYNC, NULL, 0);
    if (len > 0) {
        #ifdef ESP_PLATFORM
        uart_write_bytes(UART_NUM, packet, len);
        #endif
        ESP_LOGI(TAG, "Sent SYNC test packet (%d bytes)", (int)len);
    }
    
    // 2. Control change message
    len = create_control_change_packet(packet, sizeof(packet), 0x02, 0x00, 0x64);
    if (len > 0) {
        #ifdef ESP_PLATFORM
        uart_write_bytes(UART_NUM, packet, len);
        #endif
        ESP_LOGI(TAG, "Sent CONTROL_CHANGE test packet (%d bytes)", (int)len);
    }
    
    // 3. Note on message
    len = create_note_on_packet(packet, sizeof(packet), 0x40, 0x60, 0x00);
    if (len > 0) {
        #ifdef ESP_PLATFORM
        uart_write_bytes(UART_NUM, packet, len);
        #endif
        ESP_LOGI(TAG, "Sent NOTE_ON test packet (%d bytes)", (int)len);
    }
    
    // 4. Status request
    len = create_generic_packet(packet, sizeof(packet), WaveX::Protocol::MSG_STATUS_REQUEST, NULL, 0);
    if (len > 0) {
        #ifdef ESP_PLATFORM
        uart_write_bytes(UART_NUM, packet, len);
        #endif
        ESP_LOGI(TAG, "Sent STATUS_REQUEST test packet (%d bytes)", (int)len);
    }
    
    ESP_LOGI(TAG, "Test messages sent");
}

bool UartLink::is_busy() const
{
    return m_tx_active;
}

void UartLink::set_suspended(bool suspended)
{
    m_suspended = suspended;
}

void UartLink::toggle_inversion()
{
    // Toggle RX line inversion for debugging
    static bool inverted = false;
    inverted = !inverted;
    
    #ifdef ESP_PLATFORM
    if (inverted) {
        uart_set_line_inverse(UART_NUM, UART_SIGNAL_RXD_INV);
        ESP_LOGI(TAG, "RX line inversion ENABLED");
    } else {
        uart_set_line_inverse(UART_NUM, 0);
        ESP_LOGI(TAG, "RX line inversion DISABLED");
    }
    
    // Flush any pending data after inversion change
    uart_flush_input(UART_NUM);
    #else
    ESP_LOGI(TAG, "Line inversion toggled (simulated)");
    #endif
}

void UartLink::start_rx_task()
{
    if (m_rx_task_handle) {
        ESP_LOGI(TAG, "RX task already running");
        return;
    }
    
    #ifdef ESP_PLATFORM
    xTaskCreatePinnedToCore(rx_task, "uart_rx", 4096, this, 3, &m_rx_task_handle, 1);
    if (m_rx_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create RX task");
    }
    #endif
}

void UartLink::start_tx_task()
{
    if (m_tx_task_handle) {
        ESP_LOGI(TAG, "TX task already running");
        return;
    }
    
    #ifdef ESP_PLATFORM
    xTaskCreatePinnedToCore(tx_task, "uart_tx", 4096, this, 3, &m_tx_task_handle, 1);
    if (m_tx_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create TX task");
    }
    #endif
}

void UartLink::stop_tasks()
{
    #ifdef ESP_PLATFORM
    if (m_rx_task_handle) {
        vTaskDelete(m_rx_task_handle);
        m_rx_task_handle = NULL;
    }
    
    if (m_tx_task_handle) {
        vTaskDelete(m_tx_task_handle);
        m_tx_task_handle = NULL;
    }
    #endif
    
    m_started = false;
}

esp_err_t UartLink::configure_uart()
{
    #ifdef ESP_PLATFORM
    uart_config_t uart_config = {};
    uart_config.baud_rate = BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_param_config(UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %d", ret);
        return ret;
    }

    ret = uart_set_pin(UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %d", ret);
        return ret;
    }

    ret = uart_driver_install(UART_NUM, BUFFER_SIZE, BUFFER_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %d", ret);
        return ret;
    }

    // Flush any pending noise and set a small RX timeout
    uart_flush_input(UART_NUM);
    uart_set_rx_timeout(UART_NUM, 2);
    
    ESP_LOGI(TAG, "UART configured successfully");
    #else
    ESP_LOGI(TAG, "UART configuration simulated");
    #endif
    
    return ESP_OK;
}

esp_err_t UartLink::create_tasks()
{
    #ifdef ESP_PLATFORM
    // Create TX queue
    m_tx_queue = xQueueCreate(TX_QUEUE_SIZE, BUFFER_SIZE);
    if (m_tx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return -1; // ESP_FAIL
    }
    #endif
    
    // Start tasks
    start_rx_task();
    start_tx_task();
    
    return ESP_OK;
}

esp_err_t UartLink::queue_message(const uint8_t* data, size_t length)
{
    if (!m_tx_queue || !data || length == 0) {
        return -1; // ESP_FAIL
    }
    
    // Prepare queue item: first byte is length, followed by data
    uint8_t queue_item[BUFFER_SIZE];
    queue_item[0] = (uint8_t)length;
    memcpy(&queue_item[1], data, length);
    
    #ifdef ESP_PLATFORM
    if (xQueueSend(m_tx_queue, queue_item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGI(TAG, "TX queue full, message dropped");
        return -1; // ESP_FAIL
    }
    #endif
    
    return ESP_OK;
}

// Protocol helper methods
size_t UartLink::create_control_change_packet(uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value)
{
    return WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(buffer, buffer_size, parameter, channel, value);
}

size_t UartLink::create_note_on_packet(uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel)
{
    return WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(buffer, buffer_size, note, velocity, channel);
}

size_t UartLink::create_note_off_packet(uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t channel)
{
    return WaveX::Protocol::ProtocolHandler::CreateNoteOffPacket(buffer, buffer_size, note, channel);
}

size_t UartLink::create_sample_ctrl_packet(uint8_t* buffer, size_t buffer_size, uint8_t slot, uint8_t cmd, float rate)
{
    WaveX::Protocol::SampleCtrlMessage msg;
    msg.slot = slot;
    msg.cmd = cmd;
    msg.rate = rate;
    return WaveX::Protocol::ProtocolHandler::CreateSampleCtrlPacket(buffer, buffer_size, msg);
}

size_t UartLink::create_preview_req_packet(uint8_t* buffer, size_t buffer_size, uint8_t slot, uint32_t start, uint32_t end, uint16_t decim)
{
    WaveX::Protocol::PreviewReqMessage msg;
    msg.slot = slot;
    msg.start = start;
    msg.end = end;
    msg.decim = decim;
    return WaveX::Protocol::ProtocolHandler::CreatePreviewReqPacket(buffer, buffer_size, msg);
}

size_t UartLink::create_generic_packet(uint8_t* buffer, size_t buffer_size, uint8_t msg_type, const void* payload, size_t payload_size)
{
    return WaveX::Protocol::ProtocolHandler::CreateGenericPacket(buffer, buffer_size, msg_type, payload, payload_size);
}

// Task functions
void UartLink::rx_task(void* arg)
{
    UartLink* link = static_cast<UartLink*>(arg);
    if (!link) return;
    
    ESP_LOGI(TAG, "UART RX task started");
    
    uint8_t rxbuf[BUFFER_SIZE];
    
    while (true) {
        if (link->m_suspended) {
            #ifdef ESP_PLATFORM
            vTaskDelay(pdMS_TO_TICKS(10));
            #endif
            continue;
        }
        
        #ifdef ESP_PLATFORM
        int len = uart_read_bytes(UART_NUM, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(20));
        if (len > 0) {
            ESP_LOGD(TAG, "Received %d bytes", len);
            // TODO: Process received data through packet processor
        }
        #endif
        
        #ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(1));
        #endif
    }
}

void UartLink::tx_task(void* arg)
{
    UartLink* link = static_cast<UartLink*>(arg);
    if (!link) return;
    
    ESP_LOGI(TAG, "UART TX task started");
    
    uint8_t txbuf[BUFFER_SIZE];
    
    while (true) {
        #ifdef ESP_PLATFORM
        // Wait for message in queue
        if (xQueueReceive(link->m_tx_queue, txbuf, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Extract message length from first byte
            size_t msg_len = txbuf[0];
            const uint8_t* msg_data = &txbuf[1];
            
            if (msg_len > 0 && msg_len <= BUFFER_SIZE - 1) {
                link->m_tx_active = true;
                
                // Send data via UART
                int written = uart_write_bytes(UART_NUM, msg_data, msg_len);
                if (written == msg_len) {
                    ESP_LOGD(TAG, "Sent message: %d bytes", written);
                } else {
                    ESP_LOGI(TAG, "UART write failed: expected %d, wrote %d", msg_len, written);
                }
                
                link->m_tx_active = false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
        #else
        // Simulate task delay for non-ESP builds
        break;
        #endif
    }
}
