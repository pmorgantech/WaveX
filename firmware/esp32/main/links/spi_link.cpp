#include "spi_link.h"
#include "spi_protocol/spi_protocol.h"
#include "link_config.h"
#include <string.h>

#if WAVEX_SPI_LINK_ENABLED

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "spi_link";

// ---- Queues ----------------------------------------------------------------
static spi_device_handle_t s_dev;
static ring_t   rx_q, tx_q;
static void    *rx_backing[SPI_RX_RING_SIZE], *tx_backing[SPI_TX_RING_SIZE];
static QueueHandle_t irq_queue;

// Pre-allocated packet pool (avoid malloc in ISR paths)
static pkt_t rx_pool[SPI_POOL_SIZE], tx_pool[SPI_POOL_SIZE];
static ring_t free_rx, free_tx;
static void*  backing_frx[SPI_POOL_SIZE], *backing_ftx[SPI_POOL_SIZE];

// Statistics
static spi_link_stats_t s_stats = {};

static void take_pkt_pool_init() {
    ring_init(&free_rx, backing_frx, SPI_POOL_SIZE);
    ring_init(&free_tx, backing_ftx, SPI_POOL_SIZE);
    for (int i=0; i<SPI_POOL_SIZE; i++) {
        ring_push(&free_rx, &rx_pool[i]);
        ring_push(&free_tx, &tx_pool[i]);
    }
}

// ---- SPI init --------------------------------------------------------------
esp_err_t spi_link_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI link...");

    take_pkt_pool_init();
    ring_init(&rx_q, rx_backing, SPI_RX_RING_SIZE);
    ring_init(&tx_q, tx_backing, SPI_TX_RING_SIZE);

    // Initialize SPI bus
    spi_bus_config_t bus = {0};
    bus.mosi_io_num = PIN_SPI_MOSI;
    bus.miso_io_num = PIN_SPI_MISO;
    bus.sclk_io_num = PIN_SPI_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    ESP_ERROR_CHECK(spi_bus_initialize(ESP_VSPI_HOST, &bus, SPI_DMA_CHANNEL));

    // Configure SPI device
    spi_device_interface_config_t dev = {0};
    dev.mode = 0;
    dev.clock_speed_hz = SPI_CLOCK_SPEED_HZ;
    dev.spics_io_num = PIN_SPI_CS;
    dev.flags = SPI_DEVICE_NO_DUMMY;
    dev.queue_size = SPI_QUEUE_SIZE;
    ESP_ERROR_CHECK(spi_bus_add_device(ESP_VSPI_HOST, &dev, &s_dev));

    // IRQ from Daisy -> ESP (input, falling-edge)
    gpio_config_t gi = {
        .pin_bit_mask = 1ULL<<PIN_IRQ_DAISY2ESP,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&gi);

    irq_queue = xQueueCreate(8, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)PIN_IRQ_DAISY2ESP, [](void*){
        uint32_t x = 1;
        xQueueSendFromISR(irq_queue, &x, nullptr);
        s_stats.irq_count++;
    }, nullptr);

    // Optional: ESP->Daisy IRQ as output (idle high)
    gpio_set_direction((gpio_num_t)PIN_IRQ_ESP2DAISY, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_IRQ_ESP2DAISY, 1);

    ESP_LOGI(TAG, "SPI link initialized successfully");
    return ESP_OK;
}

// ---- API to app ------------------------------------------------------------
int spi_link_send(uint16_t type, const void* payload, uint16_t len)
{
    pkt_t *p = (pkt_t*)ring_pop(&free_tx);
    if (!p) return 0; // dropped
    pkt_fill(p, type, payload, len);
    ring_push(&tx_q, p);
    return len;
}

int spi_link_recv(void** out) {
    pkt_t* p = (pkt_t*)ring_pop(&rx_q);
    if (!p) return 0;
    if (out) *out = p;
    return p->h.len;
}

void spi_link_recycle(void* p, int is_rx) {
    if (is_rx) ring_push(&free_rx, p);
    else       ring_push(&free_tx, p);
}

// ---- Internals ---------------------------------------------------------------
static esp_err_t xfer_full_duplex(const void* tx, void* rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8, // len is in bytes, transaction length is in bits
        .tx_buffer = tx,
        .rx_buffer = rx
    };
    return spi_device_polling_transmit(s_dev, &t);
}

// Pull one packet from Daisy (assume IRQ is high)
static bool pull_packet_from_daisy(void)
{
    pkt_t *rx = (pkt_t*)ring_pop(&free_rx);
    if (!rx) return false;

    // Phase 1: read header (full-duplex; we can send zeros)
    uint8_t zeros[sizeof(pkt_hdr_t)] = {0};
    if (xfer_full_duplex(zeros, &rx->h, sizeof(pkt_hdr_t)) != ESP_OK) {
        ring_push(&free_rx, rx);
        return false;
    }

    if (rx->h.len > sizeof(rx->payload)) {
        ring_push(&free_rx, rx);
        return false;
    }

    // Phase 2: read payload+crc in one go (still full-duplex)
    size_t tail = rx->h.len + sizeof(uint16_t);
    uint8_t *dst = rx->payload;
    memset(dst, 0, tail); // tx zeros while reading
    if (xfer_full_duplex(dst, dst, tail) != ESP_OK) {
        ring_push(&free_rx, rx);
        return false;
    }

    // Verify CRC
    uint16_t rx_crc;
    memcpy(&rx_crc, rx->payload + rx->h.len, sizeof(uint16_t));
    rx->crc = rx_crc;
    if (pkt_crc(rx) != rx_crc) {
        s_stats.crc_errors++;
        ring_push(&free_rx, rx);
        return false;
    }

    ring_push(&rx_q, rx);
    return true;
}

// Push one packet to Daisy (or send ping if none)
static bool push_packet_to_daisy(void)
{
    pkt_t *tx = (pkt_t*)ring_pop(&tx_q);
    pkt_t dummy = {0};
    if (!tx) {
        pkt_fill(&dummy, 0x0000, nullptr, 0);
        tx = &dummy;
    }

    // Send header
    pkt_hdr_t hdr = tx->h;
    pkt_hdr_t hdr_rx; // ignored
    if (xfer_full_duplex(&hdr, &hdr_rx, sizeof(pkt_hdr_t)) != ESP_OK) return false;

    // Send payload+crc
    uint8_t tail[sizeof(tx->payload) + 2];
    memcpy(tail, tx->payload, tx->h.len);
    memcpy(tail + tx->h.len, &tx->crc, 2);
    uint8_t rx_ign[sizeof(tail)] = {0};
    if (xfer_full_duplex(tail, rx_ign, tx->h.len + 2) != ESP_OK) return false;

    if (tx != &dummy) spi_link_recycle(tx, 0);
    return true;
}

// ---- Link task: runs when Daisy IRQ asserts or periodically ----------------
static void link_task(void *arg)
{
    const TickType_t tout = pdMS_TO_TICKS(2);
    ESP_LOGI(TAG, "SPI link task started");

    while (true) {
        uint32_t sig;
        if (xQueueReceive(irq_queue, &sig, tout) == pdTRUE) {
            // Daisy says: I have data. Clock a read; also push any pending TX.
            pull_packet_from_daisy();
            if (!ring_empty(&tx_q)) push_packet_to_daisy();
        } else {
            // Idle poll / keepalive: push pending TX or ping; small chance to pull
            if (!ring_empty(&tx_q)) push_packet_to_daisy();
            pull_packet_from_daisy();
        }
    }
}

esp_err_t spi_link_start(void) {
    xTaskCreatePinnedToCore(link_task, "spi_link", 4096, nullptr, 20, nullptr, tskNO_AFFINITY);
    ESP_LOGI(TAG, "SPI link task created");
    return ESP_OK;
}

void spi_link_get_stats(spi_link_stats_t* stats) {
    if (stats) {
        memcpy(stats, &s_stats, sizeof(s_stats));
    }
}
#endif // ESP_PLATFORM

#endif // WAVEX_SPI_LINK_ENABLED
