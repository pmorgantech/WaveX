#include "spi_link.h"
#include "spi_protocol/spi_protocol.h"
#include "link_config.h"
#include "../inter_mcu.h"
#include <string.h>

#if WAVEX_SPI_LINK_ENABLED

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_timer.h"

static const char *TAG = "spi_link";

// ---- Queues ----------------------------------------------------------------
static spi_device_handle_t s_dev;
static ring_t   rx_q, tx_q;
static void    *rx_backing[SPI_RX_RING_SIZE], *tx_backing[SPI_TX_RING_SIZE];
static QueueHandle_t irq_queue;
static TimerHandle_t s_diag_timer;

// Pre-allocated packet pool (avoid malloc in ISR paths)
static pkt_t rx_pool[SPI_POOL_SIZE], tx_pool[SPI_POOL_SIZE];
static ring_t free_rx, free_tx;
static void*  backing_frx[SPI_POOL_SIZE], *backing_ftx[SPI_POOL_SIZE];

// Statistics
static spi_link_stats_t s_stats = {};

// Forward declarations
static bool push_packet_to_daisy(void);

// 5s diagnostics timer callback
static void diag_timer_cb(TimerHandle_t /*xTimer*/)
{
    int lvl_irq_in = gpio_get_level((gpio_num_t)PIN_IRQ_DAISY2ESP);
    int lvl_attn   = gpio_get_level((gpio_num_t)PIN_IRQ_ESP2DAISY);
    static uint32_t last_irq_count_logged = 0;
    uint32_t irq_cnt_snapshot = s_stats.irq_count;
    ESP_LOGI(TAG, "Diag: DaisyIRQ(level=%d) ATTN(level=%d) irq_count=%lu (+%lu)",
             lvl_irq_in, lvl_attn,
             (unsigned long)irq_cnt_snapshot,
             (unsigned long)(irq_cnt_snapshot - last_irq_count_logged));
    last_irq_count_logged = irq_cnt_snapshot;
    
    // Periodic "poke" to unstick deadlocked DMA on Daisy
    // This completes any pending DMA transfers that may be waiting for SPI clock
    ESP_LOGI(TAG, "Performing periodic SPI poke to unstick Daisy DMA...");
    push_packet_to_daisy(); // Send a ping packet to complete Daisy's DMA
}

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
    
    // Initialize configured SPI host using AUTO DMA (required on this chip)
    esp_err_t ret = spi_bus_initialize(WAVEX_ESP_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure SPI device - try different modes systematically
    spi_device_interface_config_t dev = {0};
    dev.mode = 0;  // Go back to Mode 0 (CPOL=0, CPHA=0) - most common
    dev.clock_speed_hz = 1000000;  // Reduce to 1MHz for better reliability
    dev.spics_io_num = PIN_SPI_CS;
    dev.flags = SPI_DEVICE_NO_DUMMY;
    dev.queue_size = SPI_QUEUE_SIZE;
    
    ESP_LOGI(TAG, "ESP32 SPI Config: Mode=%d, Clock=%dHz, CS=GPIO%d", 
             dev.mode, dev.clock_speed_hz, dev.spics_io_num);
    
    // Print actual pin numbers for verification
    ESP_LOGI(TAG, "ESP32 SPI Pins: SCK=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, CS=GPIO%d", 
             PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_CS);
    
    ret = spi_bus_add_device(WAVEX_ESP_SPI_HOST, &dev, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device addition failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // IRQ from Daisy -> ESP (input)
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
        uint32_t evt = 1;
        xQueueSendFromISR(irq_queue, &evt, NULL);
    }, nullptr);

    // Optional: ESP->Daisy IRQ as output (idle high)
    gpio_config_t go = {
        .pin_bit_mask = 1ULL << PIN_IRQ_ESP2DAISY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&go);
    gpio_set_level((gpio_num_t)PIN_IRQ_ESP2DAISY, 1);

    // Debug: log initial pin states
    int lvl_irq_in = gpio_get_level((gpio_num_t)PIN_IRQ_DAISY2ESP);
    int lvl_attn   = gpio_get_level((gpio_num_t)PIN_IRQ_ESP2DAISY);
    ESP_LOGI(TAG, "IRQ init: Daisy->ESP pin=%d level=%d, ESP->Daisy ATTN pin=%d level=%d",
             (int)PIN_IRQ_DAISY2ESP, lvl_irq_in, (int)PIN_IRQ_ESP2DAISY, lvl_attn);

    // If Daisy is already asserting IRQ low at startup, service it immediately
    if (lvl_irq_in == 0) {
        uint32_t x = 1;
        xQueueSend(irq_queue, &x, 0);
        ESP_LOGI(TAG, "IRQ low at init -> queued immediate service event");
    }

    // Basic connectivity test
    int miso_idle = gpio_get_level((gpio_num_t)PIN_SPI_MISO);
    ESP_LOGI(TAG, "SPI connectivity test: MISO level when idle = %d", miso_idle);
    
    // Test CS control - manually toggle CS and see if Daisy responds
    gpio_set_level((gpio_num_t)PIN_SPI_CS, 1);  // CS high (idle)
    vTaskDelay(pdMS_TO_TICKS(10));
    int miso_cs_high = gpio_get_level((gpio_num_t)PIN_SPI_MISO);
    
    gpio_set_level((gpio_num_t)PIN_SPI_CS, 0);  // CS low (active)  
    vTaskDelay(pdMS_TO_TICKS(10));
    int miso_cs_low = gpio_get_level((gpio_num_t)PIN_SPI_MISO);
    
    gpio_set_level((gpio_num_t)PIN_SPI_CS, 1);  // CS back to idle
    
    ESP_LOGI(TAG, "CS test: MISO when CS=high:%d, CS=low:%d", miso_cs_high, miso_cs_low);
    
    ESP_LOGI(TAG, "SPI link initialized successfully");

    // Start 5s diagnostics timer so we get logs even if no IRQ events yet
    s_diag_timer = xTimerCreate("spi_diag", pdMS_TO_TICKS(5000), pdTRUE, nullptr, diag_timer_cb);
    if (s_diag_timer) {
        xTimerStart(s_diag_timer, 0);
    }
    return ESP_OK;
}

// Simple function to continuously monitor MISO level
void spi_link_monitor_miso() {
    static uint32_t last_check = 0;
    uint32_t now = esp_timer_get_time() / 1000;  // Convert to ms
    
    if (now - last_check > 2000) {  // Check every 2 seconds
        int miso_level = gpio_get_level((gpio_num_t)PIN_SPI_MISO);
        ESP_LOGI(TAG, "MISO monitor: level=%d", miso_level);
        last_check = now;
    }
}

// ---- API to app ------------------------------------------------------------
int spi_link_send(uint16_t type, const void* payload, uint16_t len)
{
    pkt_t *p = (pkt_t*)ring_pop(&free_tx);
    if (!p) return 0; // dropped
    pkt_fill(p, type, payload, len);
    ring_push(&tx_q, p);
    // Wake the link task to send without polling
    uint32_t tx_evt = 2; // event: TX queued
    if (irq_queue) {
        xQueueSend(irq_queue, &tx_evt, 0);
    }
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
        .flags = 0,
        .length = len * 8, // len is in bytes, transaction length is in bits
        .tx_buffer = tx,
        .rx_buffer = rx
    };
    
    // Acquire bus, transmit, and release bus to play nicely with other devices (e.g., display)
    esp_err_t ret = spi_device_acquire_bus(s_dev, portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = spi_device_polling_transmit(s_dev, &t);
    spi_device_release_bus(s_dev);
    return ret;
}

// Pull one packet from Daisy (assume IRQ is low)
static bool pull_packet_from_daisy(void)
{
    // Wait for IRQ to be properly asserted low (Daisy has data ready)
    if (gpio_get_level((gpio_num_t)PIN_IRQ_DAISY2ESP) != 0) {
        ESP_LOGW(TAG, "IRQ not asserted, skipping read");
        return false;
    }
    
    // Give Daisy much more time to set up DMA after asserting IRQ
    vTaskDelay(pdMS_TO_TICKS(50));
    
    pkt_t *rx = (pkt_t*)ring_pop(&free_rx);
    if (!rx) {
        s_stats.rx_pool_empty++;
        return false;
    }

    // A single, full-duplex transfer for the entire fixed-size packet.
    // We send a dummy packet (or zeros) and receive one packet from Daisy.
    static pkt_t dummy_tx; // Static to avoid stack allocation in a loop
    pkt_fill(&dummy_tx, 0, NULL, 0);

    // Check MISO level before transfer
    int miso_before = gpio_get_level((gpio_num_t)PIN_SPI_MISO);
    
    ESP_LOGI(TAG, "Starting SPI transfer (MISO=%d)...", miso_before);
    
    // Debug: Show what we're sending to Daisy
    uint8_t* dummy_ptr = (uint8_t*)&dummy_tx;
    ESP_LOGI(TAG, "Sending dummy packet [0-7]: %02X %02X %02X %02X %02X %02X %02X %02X",
             dummy_ptr[0], dummy_ptr[1], dummy_ptr[2], dummy_ptr[3],
             dummy_ptr[4], dummy_ptr[5], dummy_ptr[6], dummy_ptr[7]);
    
    // Try smaller transfer first - just read 8 bytes instead of full packet
    uint8_t small_rx[8] = {0};
    uint8_t small_tx[8] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    
    ESP_LOGI(TAG, "Trying small 8-byte transfer first...");
    esp_err_t err = xfer_full_duplex(small_tx, small_rx, 8);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Small transfer result [0-7]: %02X %02X %02X %02X %02X %02X %02X %02X",
                 small_rx[0], small_rx[1], small_rx[2], small_rx[3],
                 small_rx[4], small_rx[5], small_rx[6], small_rx[7]);
    }
    
    // Now try the full packet transfer
    err = xfer_full_duplex(&dummy_tx, rx, sizeof(pkt_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI full packet xfer failed: %s", esp_err_to_name(err));
        ring_push(&free_rx, rx);
        return false;
    }

    ESP_LOGI(TAG, "SPI transfer completed, validating packet...");
    
    // Debug: Show raw bytes received 
    uint8_t* rx_ptr = (uint8_t*)rx;
    ESP_LOGI(TAG, "Received raw bytes [0-7]: %02X %02X %02X %02X %02X %02X %02X %02X",
             rx_ptr[0], rx_ptr[1], rx_ptr[2], rx_ptr[3],
             rx_ptr[4], rx_ptr[5], rx_ptr[6], rx_ptr[7]);
    
    // Debug: Log raw header data
    ESP_LOGI(TAG, "Raw packet header: type=0x%04X len=%u crc=0x%04X", 
             rx->h.type, rx->h.len, rx->crc);

    // If the header is all zeros, Daisy likely isn't primed yet; ignore quietly.
    pkt_hdr_t zero_hdr = {};
    if (memcmp(&rx->h, &zero_hdr, sizeof(zero_hdr)) == 0) {
        ESP_LOGI(TAG, "Received empty/zero packet, ignoring.");
        ring_push(&free_rx, rx);
        return false;
    }
    
    // Check for invalid length that indicates uninitialized memory
    if (rx->h.len > sizeof(rx->payload) || rx->h.len == 0xFFFF) {
        ESP_LOGW(TAG, "SPI invalid len %u > %u (likely uninitialized memory)", 
                 (unsigned)rx->h.len, (unsigned)sizeof(rx->payload));
        ring_push(&free_rx, rx);
        return false;
    }

    // Verify CRC using the shared protocol helper
    uint16_t calc_crc = pkt_crc(rx);
    if (calc_crc != rx->crc) {
        s_stats.crc_errors++;
        ESP_LOGW(TAG, "SPI CRC mismatch: calc=0x%04X recv=0x%04X type=0x%04X len=%u", 
                 (unsigned)calc_crc, (unsigned)rx->crc, rx->h.type, (unsigned)rx->h.len);
        ring_push(&free_rx, rx);
        return false;
    }

    ESP_LOGI(TAG, "Packet received successfully - Type: 0x%04X, Len: %u", rx->h.type, rx->h.len);

    // Directly handle heartbeat packets to update diagnostics
    if (rx->h.type == 0x1000 && rx->h.len >= 12) {
        ESP_LOGI(TAG, "Received heartbeat packet");
        const uint8_t *pl = rx->payload;
        uint32_t uptime_ms    = (uint32_t)pl[0] | ((uint32_t)pl[1] << 8) | ((uint32_t)pl[2] << 16) | ((uint32_t)pl[3] << 24);
        uint32_t loop_counter = (uint32_t)pl[4] | ((uint32_t)pl[5] << 8) | ((uint32_t)pl[6] << 16) | ((uint32_t)pl[7] << 24);
        uint32_t rx_total     = (uint32_t)pl[8] | ((uint32_t)pl[9] << 8) | ((uint32_t)pl[10] << 16) | ((uint32_t)pl[11] << 24);

        inter_mcu_update_backend_heartbeat(uptime_ms, rx_total, loop_counter);
        spi_link_recycle(rx, 1);
        return true;
    }

    // Handle SYNC packets
    if (rx->h.type == 0x0000) {
        ESP_LOGI(TAG, "Received SYNC packet");
        spi_link_recycle(rx, 1);
        return true;
    }

    // Recycle other packets until routing to packet processor is implemented
    ESP_LOGI(TAG, "Received packet type 0x%04X, recycling for now", rx->h.type);
    spi_link_recycle(rx, 1);
    return true;
}

// Push one packet to Daisy (or send ping if none)
static bool push_packet_to_daisy(void)
{
    pkt_t *tx = (pkt_t*)ring_pop(&tx_q);
    pkt_t dummy_rx; // Receive buffer, contents will be ignored
    
    if (!tx) {
        // No application packet queued, send a ping packet instead
        static pkt_t ping_pkt;
        pkt_fill(&ping_pkt, 0x0000, nullptr, 0);
        tx = &ping_pkt;

        if (xfer_full_duplex(tx, &dummy_rx, sizeof(pkt_t)) != ESP_OK) {
            return false;
        }
        // Don't recycle the static ping_pkt
    } else {
        // Send the application packet
        if (xfer_full_duplex(tx, &dummy_rx, sizeof(pkt_t)) != ESP_OK) {
            // If transfer fails, re-queue the packet to try again.
            // Note: This could lead to out-of-order packets if not handled carefully.
            // For now, we just recycle it to prevent leaks.
            spi_link_recycle(tx, 0);
            return false;
        }

        #if WAVEX_MCU_LINK_DEBUG
        ESP_LOGI(TAG, "Packet sent - Type: 0x%04X, Len: %u", tx->h.type, tx->h.len);
        #endif
        spi_link_recycle(tx, 0);
    }
    
    // TODO: Process dummy_rx? If Daisy sends data unsolicited while we are also
    // sending, we would receive it here. The current design assumes half-duplex
    // logic where ESP only pulls after an IRQ. This is probably fine.

    return true;
}

// ---- Link task: runs when Daisy IRQ asserts or periodically ----------------
static void link_task(void *arg)
{
    ESP_LOGI(TAG, "SPI link task started");

    // Event values: 1 = Daisy IRQ, 2 = TX queued
    while (true) {
        uint32_t evt;
        if (xQueueReceive(irq_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (evt == 1) { // Daisy IRQ event
            s_stats.irq_count++;

            // IRQ is asserted (low). Drain packets until it goes high.
            int attempts = 0;
            const int max_attempts = 16; // Try up to 16 times

            while (gpio_get_level((gpio_num_t)PIN_IRQ_DAISY2ESP) == 0) {
                if (pull_packet_from_daisy()) {
                    // Got a valid packet, reset counter
                    attempts = 0;
                } else {
                    // Got an empty packet or an error, increment counter
                    attempts++;
                    if (attempts > max_attempts) {
                        ESP_LOGW(TAG, "Failed to drain a valid packet after %d attempts, breaking.", max_attempts);
                        break;
                    }
                    // Small delay to allow Daisy's DMA to be updated
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        } else if (evt == 2) { // TX event from application
            // TX event from application: push pending TX immediately
            push_packet_to_daisy();
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

// Debug function to check current pin states
void spi_link_debug_pins() {
    int irq_level = gpio_get_level((gpio_num_t)PIN_IRQ_DAISY2ESP);
    int attn_level = gpio_get_level((gpio_num_t)PIN_IRQ_ESP2DAISY);
    int cs_level = gpio_get_level((gpio_num_t)PIN_SPI_CS);
    ESP_LOGI(TAG, "Pin Debug: IRQ_IN=%d, ATTN_OUT=%d, CS=%d", irq_level, attn_level, cs_level);
}
#endif // ESP_PLATFORM

#endif // WAVEX_SPI_LINK_ENABLED

