#pragma once

// Hardware/RTOS-only model for the real esp_spi_link.cpp translation unit.
#include "config/pin_config.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>

#define WAVEX_SPI_LINK_ENABLED 1
#define WAVEX_SPI_DMA_ENABLED 1
#define CONFIG_CACHE_L2_CACHE_LINE_SIZE 128
#define IRAM_ATTR
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using esp_err_t = int;
#define ESP_OK 0
constexpr int ESP_ERR_TIMEOUT = 1, ESP_ERR_NO_MEM = 2, ESP_ERR_INVALID_STATE = 3,
              ESP_ERR_NOT_SUPPORTED = 4;
constexpr int SPI3_HOST = 3, SPI_DMA_CH_AUTO = 0;
constexpr int MALLOC_CAP_DMA = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_8BIT = 4;
constexpr int pdTRUE = 1, pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
constexpr uint32_t pdMS_TO_TICKS(uint32_t ms) {
    return ms;
}
using SemaphoreHandle_t = int*;
using gpio_num_t = int;
using gpio_drive_cap_t = int;
constexpr int GPIO_MODE_OUTPUT = 1, GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
    uint64_t pin_bit_mask = 0;
    int mode = 0, intr_type = 0;
};
struct spi_slave_transaction_t {
    size_t length = 0, trans_len = 0;
    const void* tx_buffer = nullptr;
    void* rx_buffer = nullptr;
};
struct spi_bus_config_t {
    int mosi_io_num = 0, miso_io_num = 0, sclk_io_num = 0, quadwp_io_num = 0, quadhd_io_num = 0,
        data4_io_num = 0, data5_io_num = 0, data6_io_num = 0, data7_io_num = 0, max_transfer_sz = 0,
        intr_flags = 0;
};
struct spi_slave_interface_config_t {
    int spics_io_num = 0, mode = 0, queue_size = 0;
    void (*post_setup_cb)(spi_slave_transaction_t*) = nullptr;
    void (*post_trans_cb)(spi_slave_transaction_t*) = nullptr;
};
namespace SpiEspMock {
inline int semaphore = 0, ready = 0;
inline int64_t now_us = 0;
inline unsigned queued = 0, waits = 0, freed = 0, routed = 0, task_deletes = 0;
inline bool fail_queue = false;
inline spi_slave_transaction_t* owned = nullptr;
inline spi_slave_interface_config_t slave{};
inline void (*task)(void*) = nullptr;
inline std::function<int(spi_slave_transaction_t**)> on_wait;
inline std::function<void()> on_route;
inline bool defer_setup = false;
inline int drive = 2;
inline bool fail_drive = false;
}  // namespace SpiEspMock
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    return &SpiEspMock::semaphore;
}
inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t) {
    assert(sem && *sem == 0);  // A recursive take would deadlock on-device.
    *sem = 1;
    return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t sem) {
    assert(*sem == 1);
    *sem = 0;
}
inline void vTaskDelay(uint32_t ticks) {
    SpiEspMock::now_us += ticks * 1000;
}
inline void vTaskDelete(void*) {
    ++SpiEspMock::task_deletes;
}
inline int xTaskCreate(void (*task)(void*), const char*, unsigned, void*, int, void*) {
    SpiEspMock::task = task;
    return pdPASS;
}
inline int64_t esp_timer_get_time() {
    return SpiEspMock::now_us;
}
inline void* heap_caps_aligned_alloc(size_t alignment, size_t bytes, int) {
    return std::aligned_alloc(alignment, bytes);
}
inline int gpio_config(const gpio_config_t*) {
    return ESP_OK;
}
inline int gpio_set_level(gpio_num_t, int level) {
    SpiEspMock::ready = level;
    return ESP_OK;
}
inline int gpio_set_drive_capability(gpio_num_t pin, gpio_drive_cap_t drive) {
    assert(pin == WAVEX_ESP_SPI_MISO);
    if (SpiEspMock::fail_drive)
        return ESP_ERR_INVALID_STATE;
    SpiEspMock::drive = drive;
    return ESP_OK;
}
inline int gpio_get_drive_capability(gpio_num_t pin, gpio_drive_cap_t* drive) {
    assert(pin == WAVEX_ESP_SPI_MISO);
    *drive = SpiEspMock::drive;
    return ESP_OK;
}
inline int spi_slave_initialize(int,
                                const spi_bus_config_t*,
                                const spi_slave_interface_config_t* slave,
                                int) {
    SpiEspMock::slave = *slave;
    return ESP_OK;
}
inline int spi_slave_queue_trans(int, spi_slave_transaction_t* trans, uint32_t) {
    assert(!SpiEspMock::owned);
    if (SpiEspMock::fail_queue)
        return ESP_ERR_INVALID_STATE;
    ++SpiEspMock::queued;
    SpiEspMock::owned = trans;
    if (!SpiEspMock::defer_setup)
        SpiEspMock::slave.post_setup_cb(trans);
    return ESP_OK;
}
inline int spi_slave_get_trans_result(int, spi_slave_transaction_t** completed, uint32_t) {
    assert(SpiEspMock::owned);
    ++SpiEspMock::waits;
    assert(SpiEspMock::on_wait);
    return SpiEspMock::on_wait(completed);
}
inline int spi_slave_free(int) {
    assert(!SpiEspMock::owned);
    ++SpiEspMock::freed;
    return ESP_OK;
}
inline void inter_mcu_increment_packet_stat(uint8_t) {}
namespace WaveX {
namespace Comm {
class PacketRouter {
   public:
    void route_uart_message(uint8_t, const uint8_t*, size_t, uint8_t, uint16_t) {
        ++SpiEspMock::routed;
        if (SpiEspMock::on_route)
            SpiEspMock::on_route();
    }
    template <typename Callback>
    void set_stats_callback(Callback) {}
};
}  // namespace Comm
}  // namespace WaveX
