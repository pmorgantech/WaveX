#include "inter_mcu.h"
#include "hardware_pins.h"
#include "spi_protocol/protocol.h"
#include "esp_log.h"
#include "driver/spi_master.h"

static const char *TAG = "inter_mcu";

static spi_device_handle_t spi_handle;

esp_err_t inter_mcu_init(void) {
    esp_err_t ret;
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = WAVEX_INTER_MCU_GPIO_MOSI,
        .miso_io_num = WAVEX_INTER_MCU_GPIO_MISO,
        .sclk_io_num = WAVEX_INTER_MCU_GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 128
    };
    
    ret = spi_bus_initialize(WAVEX_INTER_MCU_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %d", ret);
        return ret;
    }
    
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = WAVEX_INTER_MCU_SPI_CLK_HZ,
        .spics_io_num = WAVEX_INTER_MCU_GPIO_CS,
        .queue_size = 7
    };
    
    ret = spi_bus_add_device(WAVEX_INTER_MCU_SPI_HOST, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "Inter-MCU SPI master initialized successfully");
    return ESP_OK;
}

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(buffer, sizeof(buffer), parameter, channel, value);
    if (len == 0) return ESP_FAIL;
    
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = buffer
    };
    return spi_device_transmit(spi_handle, &trans);
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(buffer, sizeof(buffer), note, velocity, channel);
    if (len == 0) return ESP_FAIL;
    
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = buffer
    };
    return spi_device_transmit(spi_handle, &trans);
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateNoteOffPacket(buffer, sizeof(buffer), note, channel);
    if (len == 0) return ESP_FAIL;
    
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = buffer
    };
    return spi_device_transmit(spi_handle, &trans);
} 