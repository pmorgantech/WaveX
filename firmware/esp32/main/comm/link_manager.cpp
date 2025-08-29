#include "link_manager.h"
#include "../links/uart_link.h"
#include "../links/spi_link_wrapper.h"
#include "../../shared/config/link_config.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_err.h"
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_NO_MEM -2
#define esp_err_to_name(err) "Unknown Error"
#endif
static const char* TAG = "LinkManager";

LinkManager& LinkManager::getInstance()
{
    static LinkManager instance;
    return instance;
}

LinkManager::LinkManager()
{
    m_current_link = NULL;
    m_initialized = false;
    m_started = false;
}

esp_err_t LinkManager::init()
{
    if (m_initialized) {
        ESP_LOGI(TAG, "LinkManager already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing LinkManager...");
    
    esp_err_t ret;
    
    #if WAVEX_USE_SPI_LINK
    ret = init_spi_link();
    #else
    ret = init_uart_link();
    #endif
    
    if (ret == ESP_OK) {
        m_initialized = true;
        ESP_LOGI(TAG, "LinkManager initialized successfully");
    } else {
        ESP_LOGE(TAG, "LinkManager initialization failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t LinkManager::start()
{
    if (!m_initialized) {
        ESP_LOGE(TAG, "LinkManager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (m_started) {
        ESP_LOGI(TAG, "LinkManager already started");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting LinkManager...");
    
    esp_err_t ret = m_current_link->start();
    if (ret == ESP_OK) {
        m_started = true;
        ESP_LOGI(TAG, "LinkManager started successfully");
    } else {
        ESP_LOGE(TAG, "LinkManager start failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t LinkManager::send_control_change(uint8_t parameter, uint8_t channel, uint16_t value)
{
    if (!m_started || !m_current_link) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return m_current_link->send_control_change(parameter, channel, value);
}

esp_err_t LinkManager::send_note_on(uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (!m_started || !m_current_link) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return m_current_link->send_note_on(note, velocity, channel);
}

esp_err_t LinkManager::send_note_off(uint8_t note, uint8_t channel)
{
    if (!m_started || !m_current_link) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return m_current_link->send_note_off(note, channel);
}

esp_err_t LinkManager::send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate)
{
    if (!m_started || !m_current_link) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return m_current_link->send_sample_ctrl(slot, cmd, rate);
}

esp_err_t LinkManager::send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim)
{
    if (!m_started || !m_current_link) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return m_current_link->send_preview_req(slot, start, end, decim);
}

void LinkManager::send_test_messages()
{
    if (!m_started || !m_current_link) {
        ESP_LOGE(TAG, "LinkManager not started");
        return;
    }
    
    m_current_link->send_test_messages();
}

bool LinkManager::is_busy() const
{
    if (!m_current_link) {
        return false;
    }
    
    return m_current_link->is_busy();
}

bool LinkManager::is_spi_link() const
{
    #if WAVEX_USE_SPI_LINK
    return true;
    #else
    return false;
    #endif
}

bool LinkManager::is_uart_link() const
{
    #if WAVEX_USE_UART_LINK
    return true;
    #else
    return false;
    #endif
}

ILink* LinkManager::get_current_link() const
{
    return m_current_link;
}

void LinkManager::process_received_packets()
{
    if (!m_started || !m_current_link) {
        return;
    }
    
    // For SPI links, we need to call the specific packet processing method
    if (is_spi_link()) {
        SpiLink* spi_link = static_cast<SpiLink*>(m_current_link);
        if (spi_link) {
            spi_link->process_received_packets();
        }
    }
    // For UART links, packet processing is handled by the UART link itself
}

esp_err_t LinkManager::init_spi_link()
{
    ESP_LOGI(TAG, "Initializing SPI link...");
    
    SpiLink* spi_link = new SpiLink();
    if (!spi_link) {
        ESP_LOGE(TAG, "Failed to create SPI link");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = spi_link->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI link init failed: %s", esp_err_to_name(ret));
        delete spi_link;
        return ret;
    }
    
    m_current_link = spi_link;
    ESP_LOGI(TAG, "SPI link initialized successfully");
    
    return ESP_OK;
}

esp_err_t LinkManager::init_uart_link()
{
    ESP_LOGI(TAG, "Initializing UART link...");
    
    UartLink* uart_link = new UartLink();
    if (!uart_link) {
        ESP_LOGE(TAG, "Failed to create UART link");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = uart_link->init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART link init failed: %s", esp_err_to_name(ret));
        delete uart_link;
        return ret;
    }
    
    m_current_link = uart_link;
    ESP_LOGI(TAG, "UART link initialized successfully");
    
    return ESP_OK;
}
