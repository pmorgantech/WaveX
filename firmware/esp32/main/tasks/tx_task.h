#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

// Forward declarations
class StatisticsManager;

// TX task that handles outgoing message transmission
class TxTask {
public:
    TxTask(StatisticsManager& stats);
    ~TxTask();
    
    // Task management
    esp_err_t start();
    void stop();
    bool is_running() const { 
        #ifdef ESP_PLATFORM
        return m_task_handle != NULL; 
        #else
        return m_task_handle != nullptr;
        #endif
    }
    
    // Configuration
    void set_uart_num(
        #ifdef ESP_PLATFORM
        uart_port_t uart_num
        #else
        int uart_num
        #endif
    ) { m_uart_num = uart_num; }
    void set_queue_size(size_t queue_size) { m_queue_size = queue_size; }
    
    // Message transmission
    esp_err_t send_message(const uint8_t* data, size_t length);
    void send_ping_message();
    void send_test_messages();

private:
    // Task function
    static void task_function(void* arg);
    
    // References to other components (must come first for proper initialization order)
    StatisticsManager& m_stats;
    
    // Task state
    #ifdef ESP_PLATFORM
    TaskHandle_t m_task_handle;
    QueueHandle_t m_tx_queue;
    #else
    void* m_task_handle;
    void* m_tx_queue;
    #endif
    bool m_running;
    
    // UART configuration
    #ifdef ESP_PLATFORM
    uart_port_t m_uart_num;
    #else
    int m_uart_num;
    #endif
    size_t m_queue_size;
    
    // Timing
    uint32_t m_last_ping_time;
    uint32_t m_last_test_time;
    
    // Constants
    static const uint32_t PING_INTERVAL_MS = 5000; // 5 seconds
    static const uint32_t TEST_INTERVAL_MS = 30000; // 30 seconds
};
