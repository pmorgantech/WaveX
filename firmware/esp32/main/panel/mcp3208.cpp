#include "mcp3208.h"

#include "config/hardware_config.h"
#include "esp_heap_caps.h"
#include "panel_spi.h"

#include "panel/mcp3208.hpp"
#include <cstring>
namespace wavex_panel {
namespace {
spi_device_handle_t device = nullptr;
uint8_t *tx = nullptr, *rx = nullptr;
}  // namespace
void CloseAdc() {
    if (device) {
        spi_bus_remove_device(device);
        device = nullptr;
    }
    if (tx) {
        heap_caps_free(tx);
        tx = nullptr;
    }
    if (rx) {
        heap_caps_free(rx);
        rx = nullptr;
    }
}
esp_err_t InitAdc() {
#if !WAVEX_PANEL_POTS_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    auto result = EnsureSpiBus();
    if (result != ESP_OK)
        return result;
    spi_device_interface_config_t config{};
    config.mode = 0;
    config.clock_speed_hz = WAVEX_MCP3208_CLOCK_HZ;
    config.spics_io_num = WAVEX_ESP_MCP3208_CS;
    config.cs_ena_pretrans = 1;
    config.cs_ena_posttrans = 1;
    config.queue_size = 1;
    result = spi_bus_add_device(kPanelSpiHost, &config, &device);
    if (result == ESP_OK) {
        tx = static_cast<uint8_t*>(spi_bus_dma_memory_alloc(kPanelSpiHost, 4, MALLOC_CAP_INTERNAL));
        rx = static_cast<uint8_t*>(spi_bus_dma_memory_alloc(kPanelSpiHost, 4, MALLOC_CAP_INTERNAL));
        if (!tx || !rx)
            result = ESP_ERR_NO_MEM;
    }
    if (result != ESP_OK)
        CloseAdc();
    return result;
#endif
}
esp_err_t ReadAdc(std::array<uint16_t, 8>& values) {
    if (!device || !tx || !rx)
        return ESP_ERR_INVALID_STATE;
    std::array<uint16_t, 8> next{};
    for (uint8_t channel = 0; channel < next.size(); ++channel) {
        const auto command = WaveX::Panel::Mcp3208Command(channel);
        std::memcpy(tx, command.data(), command.size());
        spi_transaction_t transaction{};
        transaction.length = transaction.rxlength = 32;
        transaction.tx_buffer = tx;
        transaction.rx_buffer = rx;
        const auto result = spi_device_transmit(device, &transaction);
        if (result != ESP_OK)
            return result;
        if (!WaveX::Panel::Mcp3208Result(rx, next[channel]))
            return ESP_ERR_INVALID_RESPONSE;
    }
    values = next;  // no partially received scan ever escapes
    return ESP_OK;
}
}  // namespace wavex_panel
