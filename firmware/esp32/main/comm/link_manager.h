#pragma once

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

// Forward declarations
class PacketProcessor;
class StatisticsManager;
class ListenersManager;

// Abstract link interface
class ILink {
public:
    virtual ~ILink() = default;
    virtual esp_err_t init() = 0;
    virtual esp_err_t start() = 0;
    virtual esp_err_t send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) = 0;
    virtual esp_err_t send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) = 0;
    virtual esp_err_t send_note_off(uint8_t note, uint8_t channel) = 0;
    virtual esp_err_t send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate) = 0;
    virtual esp_err_t send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) = 0;
    virtual void send_test_messages() = 0;
    virtual bool is_busy() const = 0;
};

// Link manager that handles link selection and coordination
class LinkManager {
public:
    static LinkManager& getInstance();
    
    esp_err_t init();
    esp_err_t start();
    esp_err_t send_control_change(uint8_t parameter, uint8_t channel, uint16_t value);
    esp_err_t send_note_on(uint8_t note, uint8_t velocity, uint8_t channel);
    esp_err_t send_note_off(uint8_t note, uint8_t channel);
    esp_err_t send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate);
    esp_err_t send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim);
    void send_test_messages();
    bool is_busy() const;
    
    // Link type queries
    bool is_spi_link() const;
    bool is_uart_link() const;
    
    // Get current link instance
    ILink* get_current_link() const;
    
    // Process any received packets from the current link
    void process_received_packets();

private:
    LinkManager();
    ~LinkManager() = default;
    LinkManager(const LinkManager&) = delete;
    LinkManager& operator=(const LinkManager&) = delete;
    
    esp_err_t init_spi_link();
    esp_err_t init_uart_link();
    
    ILink* m_current_link;
    bool m_initialized;
    bool m_started;
};
