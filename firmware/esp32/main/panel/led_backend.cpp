#include "led_backend.h"

#include "config/hardware_config.h"
#if WAVEX_PANEL_LED_BACKEND == WAVEX_PANEL_LED_TLC5947
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "pin_config.h"
#include "tlc5947_frame.h"
#endif
namespace wavex_panel {
namespace {
#if WAVEX_PANEL_LED_BACKEND == WAVEX_PANEL_LED_TLC5947
spi_device_handle_t device = nullptr;
uint8_t* dma = nullptr;
bool bus_owned = false;
constexpr auto host = static_cast<spi_host_device_t>(WAVEX_ESP_SPI2_HOST);
constexpr auto blank = static_cast<gpio_num_t>(WAVEX_ESP_TLC5947_BLANK);
constexpr auto latch = static_cast<gpio_num_t>(WAVEX_ESP_TLC5947_LAT);
void shutdown() {
    gpio_set_level(blank, 1);
    if (device) {
        spi_bus_remove_device(device);
        device = nullptr;
    }
    if (dma) {
        heap_caps_free(dma);
        dma = nullptr;
    }
    if (bus_owned) {
        spi_bus_free(host);
        bus_owned = false;
    }
}
esp_err_t write(const wavex_ui::PanelLedFrame& frame) {
    if (!device || !dma)
        return ESP_ERR_INVALID_STATE;
    if (frame.blanked)
        gpio_set_level(blank, 1);
    EncodeTlc5947(frame, dma);
    spi_transaction_t transaction{};
    transaction.length = kTlcFrameBytes * 8;
    transaction.tx_buffer = dma;
    // Interrupt-driven DMA completion; neither UI nor PCNT ISR performs SPI.
    const esp_err_t result = spi_device_transmit(device, &transaction);
    if (result != ESP_OK) {
        gpio_set_level(blank, 1);
        return result;
    }
    gpio_set_level(latch, 1);
    esp_rom_delay_us(1);
    gpio_set_level(latch, 0);
    esp_rom_delay_us(1);
    gpio_set_level(blank, frame.blanked ? 1 : 0);
    return ESP_OK;
}
esp_err_t init() {
    // External BLANK pull-up is still required before firmware starts.
    gpio_set_level(blank, 1);
    gpio_set_level(latch, 0);
    gpio_config_t pins{};
    pins.pin_bit_mask = (1ULL << blank) | (1ULL << latch);
    pins.mode = GPIO_MODE_OUTPUT;
    esp_err_t result = gpio_config(&pins);
    if (result != ESP_OK)
        return result;
    spi_bus_config_t bus{};
    bus.mosi_io_num = WAVEX_ESP_SPI2_MOSI;
    bus.miso_io_num = WAVEX_ESP_SPI2_MISO;
    bus.sclk_io_num = WAVEX_ESP_SPI2_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kTlcFrameBytes;
    result = spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO);
    if (result != ESP_OK)
        return result;  // never take over another bus owner
    bus_owned = true;
    spi_device_interface_config_t config{};
    config.mode = 0;
    config.clock_speed_hz = WAVEX_ESP_SPI2_FREQ_HZ;
    config.spics_io_num = -1;
    config.queue_size = 1;
    result = spi_bus_add_device(host, &config, &device);
    if (result == ESP_OK) {
        dma = static_cast<uint8_t*>(
            spi_bus_dma_memory_alloc(host, kTlcFrameBytes, MALLOC_CAP_INTERNAL));
        if (!dma)
            result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK)
        result = write({});  // first latch is all dark
    if (result != ESP_OK)
        shutdown();
    return result;
}
const LedBackend backend{"TLC5947", init, write, shutdown};
#else
// Deliberate replacement seam. PCA9956B addressing/current/enable setup must
// be implemented against the actual board; a stub must never report success.
esp_err_t unavailable() {
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t no_write(const wavex_ui::PanelLedFrame&) {
    return ESP_ERR_NOT_SUPPORTED;
}
void no_shutdown() {}
#if WAVEX_PANEL_LED_BACKEND == WAVEX_PANEL_LED_PCA9956B
const LedBackend backend{"PCA9956B-stub", unavailable, no_write, no_shutdown};
#else
const LedBackend backend{"disabled", unavailable, no_write, no_shutdown};
#endif
#endif
}  // namespace
const LedBackend& SelectedLedBackend() {
    return backend;
}
}  // namespace wavex_panel
