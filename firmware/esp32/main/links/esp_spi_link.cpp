#include "esp_spi_link.h"

#if WAVEX_SPI_LINK_ENABLED

#include "comm/packet_router.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "inter_mcu.h"

#include "spi_protocol/spi_transport.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

using namespace WaveX::Protocol;
using namespace WaveX::Protocol::Spi;

constexpr char kTag[] = "spi_link";
// P4 L2 cache line size is selected by sdkconfig. DMA buffers must own whole
// lines, even though the IDF driver performs the actual cache maintenance.
constexpr size_t kDmaAlignment = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
static_assert(kFrameBytes % kDmaAlignment == 0, "DMA frames must occupy whole cache lines");
SemaphoreHandle_t mutex = nullptr;  // Kept across stop/start; senders may still reference it.
TxQueue outgoing;
uint16_t next_sequence = 1;
SequenceTracker rx_sequence;
spi_link_stats_t stats{};
uint8_t* tx_dma = nullptr;
uint8_t* rx_dma = nullptr;
spi_slave_transaction_t transaction{};
std::atomic<bool> running{false};
std::atomic<bool> stop_requested{false};
bool initialized = false;
bool driver_initialized = false;
WaveX::Comm::PacketRouter* router = nullptr;
void (*packet_callback)(const uint8_t*, size_t) = nullptr;

void IRAM_ATTR SlaveReady(spi_slave_transaction_t*) {
    // queue_trans only enqueues work. This callback means hardware is loaded.
    gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_ATTN_OUT), 1);
}

void IRAM_ATTR SlaveComplete(spi_slave_transaction_t*) {
    // Every frame, including an empty or short one, consumes one READY assertion.
    gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_ATTN_OUT), 0);
}

RxResult ProcessRx(size_t actual_bytes) {
    size_t packet_bytes = 0;
    const auto result = InspectFrame(rx_dma, actual_bytes, rx_sequence, packet_bytes);
    if (result != RxResult::Packet)
        return result;
    xSemaphoreTake(mutex, portMAX_DELAY);
    auto* target = router;
    auto callback = packet_callback;
    xSemaphoreGive(mutex);
    // Dispatch outside the queue mutex: handlers may enqueue responses.
    if (target)
        target->route_packet(rx_dma, packet_bytes);
    else if (callback)
        callback(rx_dma, packet_bytes);
    return result;
}

void SlaveTask(void*) {
    while (!stop_requested.load(std::memory_order_acquire)) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool prepared = outgoing.Begin(tx_dma);
        xSemaphoreGive(mutex);
        if (!prepared)
            break;  // Internal invariant failure: retain memory rather than alias DMA.

        std::memset(rx_dma, 0, kFrameBytes);
        transaction = {};
        transaction.length = kFrameBytes * 8;
        transaction.tx_buffer = tx_dma;
        transaction.rx_buffer = rx_dma;
        const auto queued = spi_slave_queue_trans(WAVEX_ESP_SPI_HOST, &transaction, 0);
        if (queued != ESP_OK) {
            // Queue failure never transferred ownership to IDF.
            xSemaphoreTake(mutex, portMAX_DELAY);
            outgoing.Finish(false);
            xSemaphoreGive(mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Timeout is only a wait result, NOT cancellation. Keep this exact
        // descriptor, both buffers and its queue-head reservation until IDF
        // returns it. New messages stay behind even an in-flight empty frame.
        spi_slave_transaction_t* completed = nullptr;
        for (;;) {
            const auto result =
                spi_slave_get_trans_result(WAVEX_ESP_SPI_HOST, &completed, pdMS_TO_TICKS(50));
            if (result == ESP_OK && completed == &transaction)
                break;
            if (result != ESP_ERR_TIMEOUT) {
                ESP_LOGE(kTag, "Unexpected SPI completion; retaining DMA ownership");
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // trans_len is the number of bits actually clocked. length is capacity.
        const bool full_frame = completed->trans_len == kFrameBytes * 8;
        const auto rx_result = ProcessRx(full_frame ? kFrameBytes : 0);
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (outgoing.Finish(full_frame))
            ++stats.packets_sent;
        if (rx_result == RxResult::Packet)
            ++stats.packets_received;
        if (rx_result == RxResult::Invalid)
            ++stats.crc_errors;
        ++stats.irq_count;
        stats.last_activity_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        xSemaphoreGive(mutex);
    }
    running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void FreeBuffers() {
    std::free(tx_dma);
    std::free(rx_dma);
    tx_dma = nullptr;
    rx_dma = nullptr;
}

}  // namespace

extern "C" {

esp_err_t spi_link_init() {
#if !WAVEX_SPI_DMA_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    // Lifecycle functions are called serially by the application owner.
    if (!mutex)
        mutex = xSemaphoreCreateMutex();
    if (!mutex)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (initialized) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    tx_dma = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        kDmaAlignment, kFrameBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    rx_dma = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        kDmaAlignment, kFrameBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!tx_dma || !rx_dma) {
        FreeBuffers();
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }
    gpio_config_t gpio{};
    gpio.pin_bit_mask = uint64_t{1} << WAVEX_ESP_ATTN_OUT;
    gpio.mode = GPIO_MODE_OUTPUT;
    gpio.intr_type = GPIO_INTR_DISABLE;
    auto result = gpio_config(&gpio);
    if (result == ESP_OK)
        result = gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_ATTN_OUT), 0);
    if (result != ESP_OK) {
        FreeBuffers();
        xSemaphoreGive(mutex);
        return result;
    }
    outgoing.Reset();
    rx_sequence = SequenceTracker{};
    next_sequence = 1;
    stats = {};
    initialized = true;
    stop_requested.store(false, std::memory_order_release);
    xSemaphoreGive(mutex);
    return ESP_OK;
#endif
}

esp_err_t spi_link_start() {
    if (!mutex)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!initialized || driver_initialized || running.load(std::memory_order_acquire)) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    spi_bus_config_t bus{};
    bus.mosi_io_num = WAVEX_ESP_SPI_MOSI;
    bus.miso_io_num = WAVEX_ESP_SPI_MISO;
    bus.sclk_io_num = WAVEX_ESP_SPI_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = kFrameBytes;
    // IDF owns cache sync. No IRAM-only interrupt allocation is requested.
    bus.intr_flags = 0;
    spi_slave_interface_config_t slave{};
    slave.spics_io_num = WAVEX_ESP_SPI_CS;
    slave.mode = 0;
    slave.queue_size = 1;
    slave.post_setup_cb = SlaveReady;
    slave.post_trans_cb = SlaveComplete;
    const auto result = spi_slave_initialize(WAVEX_ESP_SPI_HOST, &bus, &slave, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        xSemaphoreGive(mutex);
        return result;
    }
    driver_initialized = true;
    stop_requested.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);
    if (xTaskCreate(SlaveTask, "spi_slave", 16384, nullptr, 5, nullptr) != pdPASS) {
        running.store(false, std::memory_order_release);
        if (spi_slave_free(WAVEX_ESP_SPI_HOST) == ESP_OK)
            driver_initialized = false;
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(mutex);
    return ESP_OK;
}

int spi_link_send(uint16_t type, const void* payload, uint16_t len) {
    if (!mutex || type > UINT8_MAX || len > kFrameBytes - 6 || (len && !payload))
        return -1;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return -1;
    if (!initialized || stop_requested.load(std::memory_order_acquire)) {
        xSemaphoreGive(mutex);
        return -1;
    }
    uint8_t packet[kFrameBytes];
    const size_t bytes = ProtocolHandler::CreateWaveXPacket(
        packet, sizeof(packet), static_cast<MessageType>(type), payload, len, next_sequence, 0);
    const bool queued = bytes != 0 && outgoing.Push(packet, bytes);
    if (queued)
        next_sequence = NextSequence(next_sequence);
    xSemaphoreGive(mutex);
    return queued ? len : -1;
}

esp_err_t spi_link_stop() {
    if (!mutex)
        return ESP_OK;
    stop_requested.store(true, std::memory_order_release);
    // IDF provides no cancellation for a queued full-duplex slave transaction.
    // Ask the task to drain it. A disconnected master can make this time out;
    // in that case retain driver/buffers and call stop again after completion.
    const auto started = esp_timer_get_time();
    while (running.load(std::memory_order_acquire)) {
        if (esp_timer_get_time() - started >= 100000)
            return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (outgoing.Owned()) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (driver_initialized) {
        const auto result = spi_slave_free(WAVEX_ESP_SPI_HOST);
        if (result != ESP_OK) {
            xSemaphoreGive(mutex);
            return result;
        }
        driver_initialized = false;
    }
    gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_ATTN_OUT), 0);
    FreeBuffers();
    initialized = false;
    xSemaphoreGive(mutex);
    return ESP_OK;
}

int spi_link_recv(void** out) {
    if (out)
        *out = nullptr;
    return 0;  // Packets are dispatched directly by the task.
}

void spi_link_recycle(void*, int) {}

void spi_link_get_stats(spi_link_stats_t* out) {
    if (!out)
        return;
    *out = {};
    if (!mutex)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    *out = stats;
    xSemaphoreGive(mutex);
}

void spi_link_log_stats() {
    spi_link_stats_t snapshot{};
    spi_link_get_stats(&snapshot);
    ESP_LOGI(kTag,
             "SPI TX=%lu RX=%lu invalid=%lu",
             static_cast<unsigned long>(snapshot.packets_sent),
             static_cast<unsigned long>(snapshot.packets_received),
             static_cast<unsigned long>(snapshot.crc_errors));
}

bool spi_link_is_active() {
    return running.load(std::memory_order_acquire) &&
           !stop_requested.load(std::memory_order_acquire);
}

void spi_link_set_packet_callback(void (*callback)(const uint8_t*, size_t)) {
    if (mutex)
        xSemaphoreTake(mutex, portMAX_DELAY);
    packet_callback = callback;
    if (mutex)
        xSemaphoreGive(mutex);
}

void spi_link_set_packet_router(WaveX::Comm::PacketRouter* packet_router) {
    if (mutex)
        xSemaphoreTake(mutex, portMAX_DELAY);
    router = packet_router;
    if (router)
        router->set_stats_callback(
            [](uint8_t packet_type) { inter_mcu_increment_packet_stat(packet_type); });
    if (mutex)
        xSemaphoreGive(mutex);
}

}  // extern "C"

#endif  // WAVEX_SPI_LINK_ENABLED
