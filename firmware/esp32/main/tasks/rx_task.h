#pragma once

#include "../comm/packet_processor.h"
#include <stdint.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

// Forward declarations
class PacketProcessor;
class StatisticsManager;

// RX task that handles incoming data reception and processing
class RxTask {
public:
    RxTask(PacketProcessor& processor, StatisticsManager& stats);
    ~RxTask();
    
    // Task management
    esp_err_t start();
    void stop();
    void set_suspended(bool suspended);
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
    void set_buffer_size(size_t buffer_size) { m_buffer_size = buffer_size; }

private:
    // Task function
    static void task_function(void* arg);
    
    // References to other components (must come first for proper initialization order)
    PacketProcessor& m_processor;
    StatisticsManager& m_stats;
    
    // Task state
    #ifdef ESP_PLATFORM
    TaskHandle_t m_task_handle;
    #else
    void* m_task_handle;
    #endif
    bool m_running;
    volatile bool m_suspended;
    
    // UART configuration
    #ifdef ESP_PLATFORM
    uart_port_t m_uart_num;
    #else
    int m_uart_num;
    #endif
    size_t m_buffer_size;
    
    // Statistics tracking
    uint32_t m_last_status_log;
    uint32_t m_no_data_cycles;
    
    // Constants
    static const uint32_t STATUS_LOG_INTERVAL_MS = 10000; // 10 seconds
    static const uint32_t SYNC_RECOVERY_INTERVAL_MS = 15000; // 15 seconds
};
