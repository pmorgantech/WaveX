#ifndef WAVEX_SPI_LINK_WRAPPER_H
#define WAVEX_SPI_LINK_WRAPPER_H

#include "../comm/link_manager.h"
#include "spi_link.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE -1
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#endif

// TAG is defined in the including file

// SpiLink class that implements ILink interface
// This wraps the existing spi_link module to integrate with the new LinkManager
class SpiLink : public ILink {
public:
    SpiLink() : m_initialized(false), m_started(false) {}
    ~SpiLink() = default;

    esp_err_t init() override {
        if (m_initialized) {
            ESP_LOGI("SpiLink", "SPI link already initialized");
            return ESP_OK;
        }

        ESP_LOGI("SpiLink", "Initializing SPI link...");
        
        esp_err_t ret = spi_link_init();
        if (ret == ESP_OK) {
            m_initialized = true;
            ESP_LOGI("SpiLink", "SPI link initialized successfully");
        } else {
            ESP_LOGE("SpiLink", "SPI link initialization failed");
        }
        
        return ret;
    }

    esp_err_t start() override {
        if (!m_initialized) {
            ESP_LOGE("SpiLink", "SPI link not initialized");
            return ESP_ERR_INVALID_STATE;
        }
        
        if (m_started) {
            ESP_LOGI("SpiLink", "SPI link already started");
            return ESP_OK;
        }
        
        ESP_LOGI("SpiLink", "Starting SPI link...");
        
        esp_err_t ret = spi_link_start();
        if (ret == ESP_OK) {
            m_started = true;
            ESP_LOGI("SpiLink", "SPI link started successfully");
        } else {
            ESP_LOGE("SpiLink", "SPI link start failed");
        }
        
        return ret;
    }

    esp_err_t send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) override {
        ESP_LOGW("SpiLink", "send_control_change is not supported in slave mode");
        return ESP_OK;
    }

    esp_err_t send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) override {
        ESP_LOGW("SpiLink", "send_note_on is not supported in slave mode");
        return ESP_OK;
    }

    esp_err_t send_note_off(uint8_t note, uint8_t channel) override {
        ESP_LOGW("SpiLink", "send_note_off is not supported in slave mode");
        return ESP_OK;
    }

    esp_err_t send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate) override {
        ESP_LOGW("SpiLink", "send_sample_ctrl is not supported in slave mode");
        return ESP_OK;
    }

    esp_err_t send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) override {
        ESP_LOGW("SpiLink", "send_preview_req is not supported in slave mode");
        return ESP_OK;
    }

    void send_test_messages() override {
        ESP_LOGW("SpiLink", "send_test_messages is not supported in slave mode");
    }

    bool is_busy() const override {
        // For now, assume SPI link is never busy
        // This could be enhanced to check actual SPI bus status
        return false;
    }

    // Process received packets from the SPI link
    void process_received_packets() {
        if (!m_started) return;
        
        // In SPI slave mode, packets are processed directly in the slave task
        // This function is kept for API compatibility but does nothing
        // The actual packet processing happens in spi_link.cpp link_task()
        // No logging needed here since it's called frequently
    }

private:
    bool m_initialized;
    bool m_started;
};

#endif // WAVEX_SPI_LINK_WRAPPER_H
