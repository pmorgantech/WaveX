#include "rx_task.h"
#include "../comm/packet_processor.h"
#include "../comm/statistics.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char* TAG = "RxTask";
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[%s] DEBUG: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "RxTask";
#endif

RxTask::RxTask(PacketProcessor& processor, StatisticsManager& stats)
    : m_processor(processor)
    , m_stats(stats)
    , m_task_handle(NULL)
    , m_running(false)
    , m_suspended(false)
    #ifdef ESP_PLATFORM
    , m_uart_num(UART_NUM_1)
    #else
    , m_uart_num(1)
    #endif
    , m_buffer_size(512)
    , m_last_status_log(0)
    , m_no_data_cycles(0)
{
}

RxTask::~RxTask()
{
    stop();
}

esp_err_t RxTask::start()
{
    if (m_running) {
        ESP_LOGI(TAG, "RX task already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting RX task...");
    
    #ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(task_function, "rx_task", 4096, this, 3, &m_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        return -1; // ESP_FAIL
    }
    #endif
    
    m_running = true;
    ESP_LOGI(TAG, "RX task started successfully");
    
    return ESP_OK;
}

void RxTask::stop()
{
    if (!m_running) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping RX task...");
    
    #ifdef ESP_PLATFORM
    if (m_task_handle) {
        vTaskDelete(m_task_handle);
        m_task_handle = NULL;
    }
    #endif
    
    m_running = false;
    ESP_LOGI(TAG, "RX task stopped");
}

void RxTask::set_suspended(bool suspended)
{
    m_suspended = suspended;
}

void RxTask::task_function(void* arg)
{
    RxTask* task = static_cast<RxTask*>(arg);
    if (!task) return;
    
    ESP_LOGI(TAG, "RX task function started");
    
    uint8_t rxbuf[512];
    uint32_t last_sync_recovery = 0;
    
    while (task->m_running) {
        if (task->m_suspended) {
            ESP_LOGD(TAG, "RX task suspended, waiting...");
            #ifdef ESP_PLATFORM
            vTaskDelay(pdMS_TO_TICKS(10));
            #endif
            continue;
        }
        
        #ifdef ESP_PLATFORM
        // Read data from UART
        int len = uart_read_bytes(task->m_uart_num, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(20));
        if (len > 0) {
            task->m_no_data_cycles = 0;  // Reset no-data counter
            
            // Process received data through packet processor
            task->m_processor.process_data(rxbuf, len);
            
        } else {
            // No data received - track this for debugging
            task->m_no_data_cycles++;
        }
        
        // Periodic status logging (every 10 seconds)
        uint32_t now = xTaskGetTickCount();
        if (now - task->m_last_status_log > pdMS_TO_TICKS(10000)) {
            // Check UART buffer status
            size_t buffered = 0;
            uart_get_buffered_data_len(task->m_uart_num, &buffered);
            
            ESP_LOGI(TAG, "RX Status: raw_bytes=%lu, parsed_frames=%lu, buffered=%u", 
                    (unsigned long)task->m_processor.get_total_raw_bytes(),
                    (unsigned long)task->m_processor.get_total_rx_bytes(),
                    (unsigned)buffered);
            
            // Debug: warn if still no parsed frames after some time and trigger sync recovery
            if (task->m_processor.get_total_rx_bytes() == 0 && 
                task->m_processor.get_total_raw_bytes() > 1000 && 
                (now - last_sync_recovery) > pdMS_TO_TICKS(15000)) {
                ESP_LOGW(TAG, "No valid frames parsed despite receiving %lu raw bytes - triggering sync recovery", 
                        (unsigned long)task->m_processor.get_total_raw_bytes());
                uart_flush_input(task->m_uart_num);
                last_sync_recovery = now;
            }
            
            task->m_last_status_log = now;
            task->m_no_data_cycles = 0;  // Reset for next period
        }
        
        // Small delay to prevent busy waiting
        vTaskDelay(pdMS_TO_TICKS(1));
        #else
        // Simulate task for non-ESP builds
        break;
        #endif
    }
    
    ESP_LOGI(TAG, "RX task function ended");
}
