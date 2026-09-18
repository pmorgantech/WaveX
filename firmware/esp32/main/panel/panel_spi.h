#pragma once
#include "driver/spi_master.h"
#include "pin_config.h"
namespace wavex_panel {
constexpr auto kPanelSpiHost = static_cast<spi_host_device_t>(WAVEX_ESP_SPI2_HOST);
// panel_task only. Bus lifetime exceeds every LED/ADC device lifetime.
esp_err_t EnsureSpiBus();
void CloseSpiBus();
}  // namespace wavex_panel
