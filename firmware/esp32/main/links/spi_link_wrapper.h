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
        if (!m_started) {
            return ESP_ERR_INVALID_STATE;
        }
        
        // Create a simple packet for control change
        uint8_t payload[4] = {
            static_cast<uint8_t>(0x01),  // Control change message type
            parameter,
            channel,
            static_cast<uint8_t>(value & 0xFF)
        };
        
        int result = spi_link_send(0x0001, payload, 4);
        return (result >= 0) ? ESP_OK : ESP_FAIL;
    }

    esp_err_t send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) override {
        if (!m_started) {
            return ESP_ERR_INVALID_STATE;
        }
        
        uint8_t payload[3] = {
            static_cast<uint8_t>(0x02),  // Note on message type
            note,
            velocity
        };
        
        int result = spi_link_send(0x0002, payload, 3);
        return (result >= 0) ? ESP_OK : ESP_FAIL;
    }

    esp_err_t send_note_off(uint8_t note, uint8_t channel) override {
        if (!m_started) {
            return ESP_ERR_INVALID_STATE;
        }
        
        uint8_t payload[3] = {
            static_cast<uint8_t>(0x03),  // Note off message type
            note,
            static_cast<uint8_t>(0x00)   // Velocity 0 for note off
        };
        
        int result = spi_link_send(0x0003, payload, 3);
        return (result >= 0) ? ESP_OK : ESP_FAIL;
    }

    esp_err_t send_sample_ctrl(uint8_t slot, uint8_t cmd, float rate) override {
        if (!m_started) {
            return ESP_ERR_INVALID_STATE;
        }
        
        // Convert float to bytes safely
        union {
            float f;
            uint8_t bytes[4];
        } rate_union;
        rate_union.f = rate;
        
        uint8_t payload[6] = {
            static_cast<uint8_t>(0x04),  // Sample control message type
            slot,
            cmd,
            rate_union.bytes[0],
            rate_union.bytes[1],
            rate_union.bytes[2]
        };
        
        int result = spi_link_send(0x0004, payload, 6);
        return (result >= 0) ? ESP_OK : ESP_FAIL;
    }

    esp_err_t send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) override {
        if (!m_started) {
            return ESP_ERR_INVALID_STATE;
        }
        
        uint8_t payload[12] = {
            static_cast<uint8_t>(0x05),  // Preview request message type
            slot,
            static_cast<uint8_t>(start & 0xFF),
            static_cast<uint8_t>((start >> 8) & 0xFF),
            static_cast<uint8_t>((start >> 16) & 0xFF),
            static_cast<uint8_t>((start >> 24) & 0xFF),
            static_cast<uint8_t>(end & 0xFF),
            static_cast<uint8_t>((end >> 8) & 0xFF),
            static_cast<uint8_t>((end >> 16) & 0xFF),
            static_cast<uint8_t>((end >> 24) & 0xFF),
            static_cast<uint8_t>(decim & 0xFF),
            static_cast<uint8_t>((decim >> 8) & 0xFF)
        };
        
        int result = spi_link_send(0x0005, payload, 12);
        return (result >= 0) ? ESP_OK : ESP_FAIL;
    }

    void send_test_messages() override {
        if (!m_started) {
            ESP_LOGE("SpiLink", "SPI link not started");
            return;
        }
        
        ESP_LOGI("SpiLink", "Sending test messages via SPI link...");
        
        // Send a simple test message
        uint8_t test_payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        spi_link_send(0x00FF, test_payload, 4);
    }

    bool is_busy() const override {
        // For now, assume SPI link is never busy
        // This could be enhanced to check actual SPI bus status
        return false;
    }

    // Process received packets from the SPI link
    void process_received_packets() {
        if (!m_started) return;
        
        void* pkt_payload = nullptr;
        while (spi_link_recv(&pkt_payload) > 0) {
            if (pkt_payload) {
                // Process the packet through the packet processor
                // This will be called by the link manager
                spi_link_recycle(pkt_payload, 1);
            }
        }
    }

private:
    bool m_initialized;
    bool m_started;
};

#endif // WAVEX_SPI_LINK_WRAPPER_H
