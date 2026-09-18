#include "panel_spi.h"

#include "config/hardware_config.h"
namespace wavex_panel {
namespace {
bool owned = false;
}
esp_err_t EnsureSpiBus() {
    if (owned)
        return ESP_OK;
    spi_bus_config_t bus{};
    bus.mosi_io_num = WAVEX_ESP_SPI2_MOSI;
    bus.miso_io_num = WAVEX_ESP_SPI2_MISO;
    bus.sclk_io_num = WAVEX_ESP_SPI2_SCLK;
    bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = WAVEX_LED_CHANNELS * 12 / 8;
    const auto error = spi_bus_initialize(kPanelSpiHost, &bus, SPI_DMA_CH_AUTO);
    owned = error == ESP_OK;
    return error;  // INVALID_STATE is not permission to take over somebody else's bus
}
void CloseSpiBus() {
    if (owned && spi_bus_free(kPanelSpiHost) == ESP_OK)
        owned = false;
}
}  // namespace wavex_panel
