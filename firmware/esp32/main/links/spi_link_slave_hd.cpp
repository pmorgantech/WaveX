#include "spi_link.h"
#include "spi_protocol/spi_protocol.h"
#include "link_config.h"
#include "../inter_mcu.h"
#include <string.h>
#include <assert.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// *** HD driver ***
#include "driver/spi_slave_hd.h"   // requires REQUIRES esp_driver_spi in CMake
#include "driver/gpio.h"
#endif

// -----------------------------
// Packet & CRC
// -----------------------------
static const char *TAG = "spi_link_hd";

#define CTRL_PKT_SIZE 32

typedef struct __attribute__((packed)) {
    uint8_t  type, flags, seq, len;
    uint8_t  payload[26];
    uint16_t crc; // CRC16-CCITT over first 30 bytes
} ctrl_pkt_t;

static uint16_t wavex_crc16(const uint8_t* d, int n)
{
    uint16_t crc=0xFFFF;
    for (int i=0;i<n;i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b=0;b<8;b++) crc = (crc & 0x8000) ? (uint16_t)((crc<<1) ^ 0x1021) : (uint16_t)(crc<<1);
    }
    return crc;
}

static void handle_ctrl_packet(const ctrl_pkt_t* p)
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "RX pkt: type=0x%02X len=%u seq=%u flags=0x%02X",
              p->type, p->len, p->seq, p->flags);

    if (p->type == 0x90 && p->len >= 4) {
        uint16_t l = (p->payload[0] | (p->payload[1]<<8));
        uint16_t r = (p->payload[2] | (p->payload[3]<<8));
        ESP_LOGI(TAG, "METER L=%u R=%u (Q15)", l, r);
        // inter_mcu_update_backend_meters(l, r); // optional helper if you have one
    } else if (p->type == 0x91 && p->len >= 12) {
        uint32_t uptime = (uint32_t)p->payload[0] | ((uint32_t)p->payload[1] << 8) |
                          ((uint32_t)p->payload[2] << 16) | ((uint32_t)p->payload[3] << 24);
        uint32_t loop_counter = (uint32_t)p->payload[4] | ((uint32_t)p->payload[5] << 8) |
                                ((uint32_t)p->payload[6] << 16) | ((uint32_t)p->payload[7] << 24);
        uint32_t rx_total = (uint32_t)p->payload[8] | ((uint32_t)p->payload[9] << 8) |
                            ((uint32_t)p->payload[10] << 16) | ((uint32_t)p->payload[11] << 24);
        inter_mcu_update_backend_heartbeat(uptime, rx_total, loop_counter);
    }
#else
    (void)p;
#endif
}

#if WAVEX_SPI_LINK_ENABLED && defined(ESP_PLATFORM)

// -----------------------------
// Stats (optional)
// -----------------------------
static spi_link_stats_t s_stats = {0};

// -----------------------------
// HD queues, buffers, descs
// -----------------------------
#ifndef WAVEX_ESP_SPI_HOST
#define WAVEX_ESP_SPI_HOST  SPI3_HOST   // keep link on SPI3; keep ADC/LED on SPI2 master
#endif

// You already defined these in your board header:
#ifndef WAVEX_ESP_SPI_SCLK
#  error "Define WAVEX_ESP_SPI_SCLK / MOSI / MISO / CS in link_config.h or board header"
#endif

// Depths
#define RX_DESC_COUNT  4        // how many RX DMA descriptors
#define TX_DESC_COUNT  2        // double-buffer TX so we can update safely

// DMA-capable buffers (aligned, internal)
static uint8_t *rxbuf[RX_DESC_COUNT];
static uint8_t *txbuf[TX_DESC_COUNT];

static spi_slave_hd_data_t rx_desc[RX_DESC_COUNT];
static spi_slave_hd_data_t tx_desc[TX_DESC_COUNT];

static volatile uint8_t tx_seq_echo = 0;

// -----------------------------
// Helpers
// -----------------------------
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void make_resp_packet(ctrl_pkt_t *out, uint8_t echo_seq)
{
    memset(out, 0, sizeof(*out));
    out->type = 0x81;   // RESP
    out->seq  = echo_seq;
    out->len  = 0;
    out->crc  = wavex_crc16((const uint8_t*)out, 30);
}

// -----------------------------
// Init
// -----------------------------
esp_err_t spi_link_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI Slave HD");

    // Allocate DMA-capable buffers (multiple of 4 for RX per driver requirement)
    for (int i=0;i<RX_DESC_COUNT;i++) {
        rxbuf[i] = (uint8_t*)heap_caps_malloc(CTRL_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!rxbuf[i]) return ESP_ERR_NO_MEM;
        memset(rxbuf[i], 0, CTRL_PKT_SIZE);
    }
    for (int i=0;i<TX_DESC_COUNT;i++) {
        txbuf[i] = (uint8_t*)heap_caps_malloc(CTRL_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!txbuf[i]) return ESP_ERR_NO_MEM;
        memset(txbuf[i], 0, CTRL_PKT_SIZE);
        make_resp_packet((ctrl_pkt_t*)txbuf[i], 0);
    }

    // Bus config (pins via matrix)
    spi_bus_config_t bus = {
        .mosi_io_num     = WAVEX_ESP_SPI_MOSI,
        .miso_io_num     = WAVEX_ESP_SPI_MISO,
        .sclk_io_num     = WAVEX_ESP_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = CTRL_PKT_SIZE, // we transfer 32-B control frames
    };

    // HD slot config — Mode 0, 8/8/8 bits for cmd/addr/dummy (master must send these)
    spi_slave_hd_slot_config_t slot = {
        .mode         = 0,  // CPOL=0, CPHA=0
        .spics_io_num = WAVEX_ESP_SPI_CS,
        .flags        = 0,
        .command_bits = 8,
        .address_bits = 8,
        .dummy_bits   = 8,
        .queue_size   = RX_DESC_COUNT + TX_DESC_COUNT,
        .dma_chan     = SPI_DMA_CH_AUTO,     // you can force CH1/CH2 if AUTO causes conflicts
        .cb_config    = {0},                 // no ISR callbacks for now
    };

    // Initialize Slave HD (exclusively grabs this SPI host)
    esp_err_t r = spi_slave_hd_init(WAVEX_ESP_SPI_HOST, &bus, &slot);
    ESP_LOGI(TAG, "spi_slave_hd_init -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return r;

    // Pre-build RX descriptors and queue them
    for (int i=0;i<RX_DESC_COUNT;i++) {
        rx_desc[i].data      = rxbuf[i];
        rx_desc[i].len       = CTRL_PKT_SIZE;        // multiple of 4 for RX
        rx_desc[i].trans_len = 0;
        rx_desc[i].flags     = 0;
        rx_desc[i].arg       = (void*)(intptr_t)i;
        ESP_ERROR_CHECK(spi_slave_hd_queue_trans(WAVEX_ESP_SPI_HOST, SPI_SLAVE_CHAN_RX, &rx_desc[i], portMAX_DELAY));
    }

    // Queue one TX descriptor so the master can RDDMA immediately
    for (int i=0;i<TX_DESC_COUNT;i++) {
        tx_desc[i].data      = txbuf[i];
        tx_desc[i].len       = CTRL_PKT_SIZE;
        tx_desc[i].trans_len = 0;
        tx_desc[i].flags     = 0;
        tx_desc[i].arg       = (void*)(intptr_t)i;
        // Queue both so we always have data ready to read
        ESP_ERROR_CHECK(spi_slave_hd_queue_trans(WAVEX_ESP_SPI_HOST, SPI_SLAVE_CHAN_TX, &tx_desc[i], portMAX_DELAY));
    }

    ESP_LOGI(TAG, "HD slave ready. Pins: SCK=%d MOSI=%d MISO=%d CS=%d  (Mode0, cmd/addr/dummy=8/8/8)",
            WAVEX_ESP_SPI_SCLK, WAVEX_ESP_SPI_MOSI, WAVEX_ESP_SPI_MISO, WAVEX_ESP_SPI_CS);
    return ESP_OK;
}

// -----------------------------
// Link task (waits for WRDMA segments to finish)
// -----------------------------
static void link_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SPI Slave HD link task start");
    uint32_t last_log = now_ms();

    while (true) {
        // Block until one RX desc finishes (master did WRDMA + WR_DONE)
        spi_slave_hd_data_t *done_rx = NULL;
        esp_err_t r = spi_slave_hd_get_trans_res(WAVEX_ESP_SPI_HOST, SPI_SLAVE_CHAN_RX, &done_rx, portMAX_DELAY);
        if (r != ESP_OK || !done_rx) {
            ESP_LOGW(TAG, "get_trans_res(RX) -> %s", esp_err_to_name(r));
            continue;
        }

        s_stats.irq_count++;
        s_stats.last_activity_ms = now_ms();

        // Process RX data (done_rx->trans_len may be <= len; HD can segment)
        if (done_rx->trans_len >= CTRL_PKT_SIZE) {
            ctrl_pkt_t *rx = (ctrl_pkt_t*)done_rx->data;
            uint16_t calc = wavex_crc16((uint8_t*)rx, 30);
            if (calc == rx->crc) {
                s_stats.packets_received++;
                tx_seq_echo = rx->seq;
                handle_ctrl_packet(rx);
            } else {
                s_stats.crc_errors++;
                ESP_LOGW(TAG, "CRC mismatch exp=0x%04X got=0x%04X", calc, rx->crc);
            }
        } else {
            ESP_LOGW(TAG, "Short RX: %u bytes", (unsigned)done_rx->trans_len);
        }

        // Prepare the *next* response in a TX buffer that is NOT in-flight.
        // We have two TX descriptors (index 0 & 1). Try to reclaim one that has completed.
        spi_slave_hd_data_t *done_tx = NULL;
        if (spi_slave_hd_get_trans_res(WAVEX_ESP_SPI_HOST, SPI_SLAVE_CHAN_TX, &done_tx, 0) == ESP_OK && done_tx) {
            // done_tx->data is safe to modify now
            ctrl_pkt_t *txp = (ctrl_pkt_t*)done_tx->data;
            make_resp_packet(txp, tx_seq_echo);
            // Re-queue TX so master can RDDMA it next
            ESP_ERROR_CHECK(spi_slave_hd_queue_trans(WAVEX_ESP_SPI_HOST, SPI_SLAVE_CHAN_TX, done_tx, portMAX_DELAY));
        }
        // Re-queue the RX descriptor so we can receive the next packet
        ESP_ERROR_CHECK(spi_slave_hd_queue_trans(WAVEX_ESP_SPI_HOST, SPI_SLAVE_CHAN_RX, done_rx, portMAX_DELAY));

        // Occasional alive log
        uint32_t now = now_ms();
        if (now - last_log > 10000) {
            ESP_LOGI(TAG, "Alive: IRQ=%lu RX=%lu CRCerr=%lu",
                     (unsigned long)s_stats.irq_count,
                     (unsigned long)s_stats.packets_received,
                     (unsigned long)s_stats.crc_errors);
            last_log = now;
        }
    }
}

// -----------------------------
// Public API
// -----------------------------
esp_err_t spi_link_start(void)
{
    ESP_LOGI(TAG, "Starting SPI link task (HD)");
    BaseType_t ok = xTaskCreate(link_task, "spi_hd_rx", 8192, NULL, 20, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool spi_link_is_active(void)
{
    return (s_stats.irq_count > 0) || (s_stats.packets_received > 0);
}

void spi_link_get_stats(spi_link_stats_t *pstats)
{
    if (pstats) memcpy(pstats, &s_stats, sizeof(s_stats));
}

#endif // WAVEX_SPI_LINK_ENABLED && ESP_PLATFORM
