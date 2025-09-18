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

// *** Regular SPI slave driver ***
#include "driver/spi_slave.h"     // requires REQUIRES esp_driver_spi in CMake
#include "driver/gpio.h"
#endif

// -----------------------------
// Packet & CRC
// -----------------------------
static const char *TAG = "spi_link";

#define CTRL_PKT_SIZE 64 // Must be 64 for P4 DMA alignment

typedef struct __attribute__((packed)) {
    uint8_t  type, flags, seq, len;
    uint8_t  payload[26];
    uint16_t crc; // CRC16-CCITT over first 30 bytes
    uint8_t  padding[32];
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
    #if WAVEX_MCU_LINK_PACKET_DEBUG
    ESP_LOGI(TAG, "RX pkt: type=0x%02X len=%u seq=%u flags=0x%02X",
              p->type, p->len, p->seq, p->flags);
    #endif

    // Update packet statistics based on packet type
    if (p->type == 0x90) {
        // Meter data packet
        inter_mcu_increment_packet_stat(0x0D); // METER_PUSH packet type
    } else if (p->type == 0x91) {
        // Heartbeat packet
        inter_mcu_increment_packet_stat(0x0F); // HEARTBEAT packet type
    } else {
        // Unknown packet type
        inter_mcu_increment_packet_stat(0xFF); // UNKNOWN packet type
    }

    if (p->type == 0x90 && p->len >= 8) {
        uint16_t q_rmsL = (p->payload[0] | (p->payload[1]<<8));
        uint16_t q_rmsR = (p->payload[2] | (p->payload[3]<<8));
        uint16_t q_pkL  = (p->payload[4] | (p->payload[5]<<8));
        uint16_t q_pkR  = (p->payload[6] | (p->payload[7]<<8));
        
        // Convert Q15 to float (divide by 32767.0f)
        float rms_left = (float)q_rmsL / 32767.0f;
        float rms_right = (float)q_rmsR / 32767.0f;
        float peak_left = (float)q_pkL / 32767.0f;
        float peak_right = (float)q_pkR / 32767.0f;
        
        #if WAVEX_MCU_LINK_PACKET_DEBUG
        ESP_LOGI(TAG, "METER L=%.3f R=%.3f PEAK_L=%.3f PEAK_R=%.3f", rms_left, rms_right, peak_left, peak_right);
        #endif
        inter_mcu_update_backend_meters(rms_left, rms_right, peak_left, peak_right);
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

static spi_slave_transaction_t rx_trans[RX_DESC_COUNT];
static spi_slave_transaction_t tx_trans[TX_DESC_COUNT];

static volatile uint8_t tx_seq_echo = 0;
static int s_next_tx_desc_idx = 0;

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
    ESP_LOGI(TAG, "Initializing Regular SPI Slave");

    // Allocate DMA-capable buffers
    for (int i=0;i<RX_DESC_COUNT;i++) {
        rxbuf[i] = (uint8_t*)heap_caps_aligned_alloc(4, CTRL_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!rxbuf[i]) return ESP_ERR_NO_MEM;
        memset(rxbuf[i], 0, CTRL_PKT_SIZE);
    }
    for (int i=0;i<TX_DESC_COUNT;i++) {
        txbuf[i] = (uint8_t*)heap_caps_aligned_alloc(4, CTRL_PKT_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
        .max_transfer_sz = CTRL_PKT_SIZE,
        .flags           = SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MISO | SPICOMMON_BUSFLAG_GPIO_PINS,
    };

    // Regular SPI slave config — Mode 0
    spi_slave_interface_config_t slave_config = {
        .spics_io_num = WAVEX_ESP_SPI_CS,
        .flags        = 0,
        .queue_size   = RX_DESC_COUNT,
        .mode         = 0,  // CPOL=0, CPHA=0
        .post_setup_cb = NULL,
        .post_trans_cb = NULL
    };

    // Initialize SPI bus
    esp_err_t r = spi_slave_initialize(WAVEX_ESP_SPI_HOST, &bus, &slave_config, SPI_DMA_CH_AUTO);
    ESP_LOGI(TAG, "spi_slave_initialize -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return r;

    // Initialize transaction structures
    for (int i=0;i<RX_DESC_COUNT;i++) {
        memset(&rx_trans[i], 0, sizeof(spi_slave_transaction_t));
        rx_trans[i].rx_buffer = rxbuf[i];
        rx_trans[i].length = CTRL_PKT_SIZE * 8;  // Length in bits
    }

    for (int i=0;i<TX_DESC_COUNT;i++) {
        memset(&tx_trans[i], 0, sizeof(spi_slave_transaction_t));
        tx_trans[i].tx_buffer = txbuf[i];
        tx_trans[i].length = CTRL_PKT_SIZE * 8;  // Length in bits
    }

    ESP_LOGI(TAG, "Regular SPI slave ready. Pins: SCK=%d MOSI=%d MISO=%d CS=%d  (Mode0)",
            WAVEX_ESP_SPI_SCLK, WAVEX_ESP_SPI_MOSI, WAVEX_ESP_SPI_MISO, WAVEX_ESP_SPI_CS);
    return ESP_OK;
}

// -----------------------------
// Link task (waits for SPI transactions)
// -----------------------------
static void link_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Regular SPI Slave link task start");
    uint32_t last_log = now_ms();
    int current_rx_idx = 0;

    while (true) {
        // Queue next RX transaction
        spi_slave_transaction_t *rx_trans_ptr = &rx_trans[current_rx_idx];
        esp_err_t r = spi_slave_queue_trans(WAVEX_ESP_SPI_HOST, rx_trans_ptr, portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "spi_slave_queue_trans failed: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Wait for transaction to complete
        spi_slave_transaction_t *completed_trans = NULL;
        r = spi_slave_get_trans_result(WAVEX_ESP_SPI_HOST, &completed_trans, pdMS_TO_TICKS(3000));
        if (r == ESP_ERR_TIMEOUT) {
            // Log periodically to show we're waiting for SPI transactions
            uint32_t now = now_ms();
            if (now - last_log > 5000) {
                #if WAVEX_MCU_LINK_PACKET_DEBUG
                ESP_LOGI(TAG, "Waiting for SPI transactions from master...");
                #endif
                last_log = now;
            }
            continue;
        } else if (r != ESP_OK || !completed_trans) {
            ESP_LOGW(TAG, "spi_slave_get_trans_result -> %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else {
            // A transaction was received!
            #if WAVEX_MCU_LINK_PACKET_DEBUG
            ESP_LOGI(TAG, "SPI RX transaction completed, len=%d bits", completed_trans->trans_len);
            #endif
            
            // Process the received packet data
            ctrl_pkt_t *rxp = (ctrl_pkt_t*)completed_trans->rx_buffer;
            handle_ctrl_packet(rxp);
            s_stats.packets_received++;
            
            // Move to next RX buffer
            current_rx_idx = (current_rx_idx + 1) % RX_DESC_COUNT;
        }

        // Occasional alive log
        uint32_t now = now_ms();
        if (now - last_log > 10000) {
            ESP_LOGI(TAG, "Alive: RX=%lu CRCerr=%lu",
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
