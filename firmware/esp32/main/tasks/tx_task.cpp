#include "tx_task.h"
#include "../comm/statistics.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
static const char* TAG = "TxTask";
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[%s] DEBUG: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "TxTask";
#endif

TxTask::TxTask(StatisticsManager& stats)
    : m_stats(stats)
    , m_task_handle(NULL)
    , m_tx_queue(NULL)
    , m_running(false)
    #ifdef ESP_PLATFORM
    , m_uart_num(UART_NUM_1)
    #else
    , m_uart_num(1)
    #endif
    , m_queue_size(16)
    , m_last_ping_time(0)
    , m_last_test_time(0)
{
}

TxTask::~TxTask()
{
    stop();
    
    if (m_tx_queue) {
        #ifdef ESP_PLATFORM
        vQueueDelete(m_tx_queue);
        #endif
        m_tx_queue = NULL;
    }
}

esp_err_t TxTask::start()
{
    if (m_running) {
        ESP_LOGI(TAG, "TX task already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting TX task...");
    
    #ifdef ESP_PLATFORM
    // Create TX queue
    m_tx_queue = xQueueCreate(m_queue_size, 512);
    if (m_tx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return -1; // ESP_FAIL
    }
    
    if (xTaskCreatePinnedToCore(task_function, "tx_task", 4096, this, 3, &m_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TX task");
        vQueueDelete(m_tx_queue);
        m_tx_queue = NULL;
        return -1; // ESP_FAIL
    }
    #endif
    
    m_running = true;
    ESP_LOGI(TAG, "TX task started successfully");
    
    return ESP_OK;
}

void TxTask::stop()
{
    if (!m_running) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping TX task...");
    
    #ifdef ESP_PLATFORM
    if (m_task_handle) {
        vTaskDelete(m_task_handle);
        m_task_handle = NULL;
    }
    #endif
    
    m_running = false;
    ESP_LOGI(TAG, "TX task stopped");
}

esp_err_t TxTask::send_message(const uint8_t* data, size_t length)
{
    if (!m_tx_queue || !data || length == 0) {
        return -1; // ESP_FAIL
    }
    
    // Prepare queue item: first byte is length, followed by data
    uint8_t queue_item[512];
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

void TxTask::send_ping_message()
{
    ESP_LOGD(TAG, "Sending ping message");
    
    // Simple ping message - just send a SYNC packet
    uint8_t ping_packet[64];
    ping_packet[0] = 0xFF;  // SYNC byte
    ping_packet[1] = 0x00;  // Message type (ping)
    ping_packet[2] = 0x00;  // Length (0)
    ping_packet[3] = 0xFF;  // End SYNC byte
    
    #ifdef ESP_PLATFORM
    uart_write_bytes(m_uart_num, ping_packet, 4);
    #endif
    
    ESP_LOGD(TAG, "Ping message sent");
}

void TxTask::send_test_messages()
{
    ESP_LOGI(TAG, "Sending test messages");
    
    // Send a variety of test messages
    uint8_t packet[64];
    
    // TODO: Create and send test packets using protocol handler
    
    ESP_LOGI(TAG, "Test messages sent");
}

void TxTask::task_function(void* arg)
{
    TxTask* task = static_cast<TxTask*>(arg);
    if (!task) return;
    
    ESP_LOGI(TAG, "TX task function started");
    
    uint8_t txbuf[512];
    
    while (task->m_running) {
        #ifdef ESP_PLATFORM
        uint32_t now = xTaskGetTickCount();
        
        // Send ping message every 5 seconds to keep communication active
        if (now - task->m_last_ping_time > pdMS_TO_TICKS(5000)) {
            task->send_ping_message();
            task->m_last_ping_time = now;
        }
        
        // Send comprehensive test messages every 30 seconds
        if (now - task->m_last_test_time > pdMS_TO_TICKS(30000)) {
            task->send_test_messages();
            task->m_last_test_time = now;
        }
        
        // Wait for message in queue
        if (xQueueReceive(task->m_tx_queue, txbuf, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Extract message length from first byte
            size_t msg_len = txbuf[0];
            const uint8_t* msg_data = &txbuf[1];
            
            if (msg_len > 0 && msg_len <= 512 - 1) {
                // Send data via UART
                int written = uart_write_bytes(task->m_uart_num, msg_data, msg_len);
                if (written == msg_len) {
                    ESP_LOGD(TAG, "Sent message: %d bytes", written);
                } else {
                    ESP_LOGI(TAG, "UART write failed: expected %d, wrote %d", msg_len, written);
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
        #else
        // Simulate task for non-ESP builds
        break;
        #endif
    }
    
    ESP_LOGI(TAG, "TX task function ended");
}
