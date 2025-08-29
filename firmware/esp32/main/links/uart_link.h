#pragma once

#include "../comm/link_manager.h"
#include "../comm/shared_packet_handler.h"
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

// UART link implementation
class UartLink : public ILink {
public:
    UartLink();
    ~UartLink();
    
    // ILink interface implementation
    esp_err_t init() override;
    esp_err_t start() override;
    esp_err_t send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) override;
    esp_err_t send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) override;
    esp_err_t send_note_off(uint8_t note, uint8_t channel) override;
    esp_err_t send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate) override;
    esp_err_t send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) override;
    void send_test_messages() override;
    bool is_busy() const override;
    
    // UART-specific methods
    void set_suspended(bool suspended);
    void toggle_inversion();
    
    // Task management
    void start_rx_task();
    void start_tx_task();
    void stop_tasks();

private:
    // UART configuration
    #ifdef ESP_PLATFORM
    static const uart_port_t UART_NUM = UART_NUM_1;
    #else
    static const int UART_NUM = 1;
    #endif
    // FALLBACK PINS ONLY - UART link should not be used when SPI is enabled
    // These pins are chosen to avoid conflicts with SPI inter-MCU communication
    static const int TX_PIN = 44;  // Changed from 17 to avoid SPI conflict
    static const int RX_PIN = 45;  // Changed from 18 to avoid SPI conflict
    static const int BAUD_RATE = 460800;
    static const int BUFFER_SIZE = 512;
    static const int TX_QUEUE_SIZE = 16;
    
    // UART state
    bool m_initialized;
    bool m_started;
    volatile bool m_suspended;
    volatile bool m_tx_active;
    
    // Task handles
    #ifdef ESP_PLATFORM
    TaskHandle_t m_rx_task_handle;
    TaskHandle_t m_tx_task_handle;
    QueueHandle_t m_tx_queue;
    #else
    void* m_rx_task_handle;
    void* m_tx_task_handle;
    void* m_tx_queue;
    #endif
    
    // Task functions
    static void rx_task(void* arg);
    static void tx_task(void* arg);
    
    // Helper methods
    esp_err_t configure_uart();
    esp_err_t create_tasks();
    esp_err_t queue_message(const uint8_t* data, size_t length);
    void send_ping_message();
    void send_comprehensive_test_messages();
    
    // Protocol helpers
    size_t create_control_change_packet(uint8_t* buffer, size_t buffer_size, uint8_t parameter, uint8_t channel, uint16_t value);
    size_t create_note_on_packet(uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t velocity, uint8_t channel);
    size_t create_note_off_packet(uint8_t* buffer, size_t buffer_size, uint8_t note, uint8_t channel);
    size_t create_sample_ctrl_packet(uint8_t* buffer, size_t buffer_size, uint8_t slot, uint8_t cmd, float rate);
    size_t create_preview_req_packet(uint8_t* buffer, size_t buffer_size, uint8_t slot, uint32_t start, uint32_t end, uint16_t decim);
    size_t create_generic_packet(uint8_t* buffer, size_t buffer_size, uint8_t msg_type, const void* payload, size_t payload_size);
};
