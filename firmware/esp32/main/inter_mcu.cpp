#include "inter_mcu.h"
#include "hardware_pins.h"
#include "spi_protocol/protocol.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "inter_mcu";

static spi_device_handle_t spi_handle;

// IRQ line from Daisy (PB0 → ESP32 GPIO)
#ifndef WAVEX_INTER_MCU_GPIO_IRQ
#define WAVEX_INTER_MCU_GPIO_IRQ   GPIO_NUM_16  // per docs/communication-protocol.md mapping
#endif

// Listeners
static wavex_meter_cb_t s_meter_cb = NULL;
static wavex_wave_chunk_cb_t s_wave_cb = NULL;
static void* s_meter_ud = NULL;
static void* s_wave_ud = NULL;
static TaskHandle_t s_rx_task_handle = nullptr;
static volatile bool s_force_poll = false;
static volatile bool s_suspended = false;
static volatile bool s_tx_active = false;

// RX task: triggered by IRQ ISR via task notification
static void inter_mcu_rx_task(void* arg) {
    static uint8_t rxbuf[sizeof(WaveX::Protocol::PacketHeader) + WaveX::Protocol::MAX_PAYLOAD_SIZE];
    static uint8_t zerobuf[sizeof(rxbuf)] = {0};
    uint32_t last_ok_ms = 0;
    while (true) {
        // Wait briefly; always poll even if IRQ is unwired
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        s_force_poll = false;
        if (s_suspended) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        // Perform a single full-length RX transaction to avoid CS toggling mid-frame
        spi_transaction_t t = {};
        t.length = (sizeof(rxbuf)) * 8;
        t.rx_buffer = rxbuf;
        t.tx_buffer = zerobuf; // clock out zeros
        s_tx_active = true;
        esp_err_t tre = spi_device_transmit(spi_handle, &t);
        s_tx_active = false;
        if (tre != ESP_OK) {
            // Back off on errors to reduce contention
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        using namespace WaveX::Protocol;
        const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(rxbuf);
        if (hdr->sync != SYNC_BYTE || hdr->length > MAX_PAYLOAD_SIZE) {
            continue;
        }
        // Validate checksum against the received payload slice
        const uint8_t* payload = rxbuf + sizeof(PacketHeader);
        if (ProtocolHandler::CalculateChecksum(payload, hdr->length) != hdr->checksum) {
            continue;
        }
        switch (hdr->type) {
            case MSG_METER_PUSH: {
                if (s_meter_cb && hdr->length == sizeof(MeterPushMessage)) {
                    MeterPushMessage m{};
                    memcpy(&m, payload, sizeof(m));
                    s_meter_cb(m.rms, m.peak, s_meter_ud);
                }
                break;
            }
            case MSG_WAVE_CHUNK: {
                if (s_wave_cb && hdr->length >= sizeof(WaveChunkMessage)) {
                    WaveChunkMessage h{};
                    memcpy(&h, payload, sizeof(h));
                    const uint16_t count = h.count;
                    if (sizeof(WaveChunkMessage) + count * sizeof(int16_t) == hdr->length) {
                        const int16_t* samples = reinterpret_cast<const int16_t*>(payload + sizeof(WaveChunkMessage));
                        s_wave_cb(h.offset, samples, count, s_wave_ud);
                    }
                }
                break;
            }
            default:
                break;
        }
        // Heartbeat log if packet seen
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_ok_ms > 1000) {
            ESP_LOGI(TAG, "Daisy alive: type=0x%02x len=%u", (unsigned)hdr->type, (unsigned)hdr->length);
            last_ok_ms = now_ms;
        }
        // Re-enable IRQ line (safe even if not wired)
        gpio_intr_enable(WAVEX_INTER_MCU_GPIO_IRQ);
    }
}

static void IRAM_ATTR inter_mcu_irq_isr(void* arg) {
    // Edge-storm guard: disable GPIO interrupt until RX task services it
    gpio_intr_disable(WAVEX_INTER_MCU_GPIO_IRQ);
    BaseType_t hp = pdFALSE;
    if (s_rx_task_handle) {
        vTaskNotifyGiveFromISR(s_rx_task_handle, &hp);
        if (hp == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

// Make init a no-op to avoid any SPI activity before LGFX panel is stable
esp_err_t inter_mcu_init(void) {
    ESP_LOGI(TAG, "inter_mcu_init(): deferred start");
    return ESP_OK;
}

esp_err_t inter_mcu_start(void) {
    esp_err_t ret;
    spi_bus_config_t bus_cfg{};
    bus_cfg.mosi_io_num = WAVEX_INTER_MCU_GPIO_MOSI;
    bus_cfg.miso_io_num = WAVEX_INTER_MCU_GPIO_MISO;
    bus_cfg.sclk_io_num = WAVEX_INTER_MCU_GPIO_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 128;
    
    // ESP32-S3 uses GDMA; must auto-allocate the DMA channel
    ret = spi_bus_initialize(WAVEX_INTER_MCU_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %d", ret);
        return ret;
    }
    
    spi_device_interface_config_t dev_cfg{};
    dev_cfg.mode = 0;
    dev_cfg.clock_speed_hz = WAVEX_INTER_MCU_SPI_CLK_HZ;
    dev_cfg.spics_io_num = WAVEX_INTER_MCU_GPIO_CS;
    dev_cfg.queue_size = 7;
    
    ret = spi_bus_add_device(WAVEX_INTER_MCU_SPI_HOST, &dev_cfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %d", ret);
        return ret;
    }
    
    // Create RX task for handling IRQ-driven reads
    if (s_rx_task_handle == NULL) {
        // Lower priority and pin to APP CPU1; UI/LGFX runs on CPU0
        xTaskCreatePinnedToCore(inter_mcu_rx_task, "inter_mcu_rx", 4096, NULL, 3, &s_rx_task_handle, 1);
    }

    // Configure IRQ pin as plain input with pulldown; do NOT enable interrupts (polling mode)
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = 1ULL << WAVEX_INTER_MCU_GPIO_IRQ;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config for IRQ pin failed: %d", ret);
        return ret;
    }
    ESP_LOGW(TAG, "Inter-MCU IRQ disabled; using polling RX on GPIO %d", (int)WAVEX_INTER_MCU_GPIO_IRQ);

    ESP_LOGI(TAG, "Inter-MCU SPI master initialized successfully (IRQ on GPIO %d)", (int)WAVEX_INTER_MCU_GPIO_IRQ);
    return ESP_OK;
}

extern "C" void inter_mcu_force_poll(void) {
    s_force_poll = true;
    if (s_rx_task_handle) {
        xTaskNotifyGive(s_rx_task_handle);
    }
}

extern "C" void inter_mcu_set_suspended(bool suspended) {
    s_suspended = suspended;
}

extern "C" bool inter_mcu_is_busy(void) {
    return s_tx_active;
}

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateControlChangePacket(buffer, sizeof(buffer), parameter, channel, value);
    if (len == 0) return ESP_FAIL;
    
    spi_transaction_t trans{};
    trans.length = (size_t)len * 8;
    trans.tx_buffer = buffer;
    return spi_device_transmit(spi_handle, &trans);
}

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateNoteOnPacket(buffer, sizeof(buffer), note, velocity, channel);
    if (len == 0) return ESP_FAIL;
    
    spi_transaction_t trans{};
    trans.length = (size_t)len * 8;
    trans.tx_buffer = buffer;
    return spi_device_transmit(spi_handle, &trans);
}

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) {
    uint8_t buffer[128];
    size_t len = WaveX::Protocol::ProtocolHandler::CreateNoteOffPacket(buffer, sizeof(buffer), note, channel);
    if (len == 0) return ESP_FAIL;
    
    spi_transaction_t trans{};
    trans.length = (size_t)len * 8;
    trans.tx_buffer = buffer;
    return spi_device_transmit(spi_handle, &trans);
} 

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) {
    uint8_t buffer[128];
    WaveX::Protocol::SampleCtrlMessage m{};
    m.slot = slot;
    m.cmd = static_cast<uint8_t>(cmd);
    m.rate = rate;
    size_t len = WaveX::Protocol::ProtocolHandler::CreateSampleCtrlPacket(buffer, sizeof(buffer), m);
    if (len == 0) return ESP_FAIL;
    spi_transaction_t trans{};
    trans.length = (size_t)len * 8;
    trans.tx_buffer = buffer;
    return spi_device_transmit(spi_handle, &trans);
}

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) {
    uint8_t buffer[128];
    WaveX::Protocol::PreviewReqMessage m{};
    m.slot = slot;
    m.start = start;
    m.end = end;
    m.decim = decim;
    size_t len = WaveX::Protocol::ProtocolHandler::CreatePreviewReqPacket(buffer, sizeof(buffer), m);
    if (len == 0) return ESP_FAIL;
    spi_transaction_t trans{};
    trans.length = (size_t)len * 8;
    trans.tx_buffer = buffer;
    return spi_device_transmit(spi_handle, &trans);
}

void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data) {
    s_meter_cb = cb; s_meter_ud = user_data;
}

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) {
    s_wave_cb = cb; s_wave_ud = user_data;
}

#else // !ESP_PLATFORM (host/lint build stubs)

// Minimal stubs for non-ESP builds so linters and host builds pass
static wavex_meter_cb_t s_meter_cb = NULL;
static wavex_wave_chunk_cb_t s_wave_cb = NULL;
static void* s_meter_ud = NULL;
static void* s_wave_ud = NULL;

esp_err_t inter_mcu_init(void) { return ESP_OK; }

esp_err_t inter_mcu_send_control_change(uint8_t parameter, uint8_t channel, uint16_t value) { (void)parameter; (void)channel; (void)value; return ESP_OK; }

esp_err_t inter_mcu_send_note_on(uint8_t note, uint8_t velocity, uint8_t channel) { (void)note; (void)velocity; (void)channel; return ESP_OK; }

esp_err_t inter_mcu_send_note_off(uint8_t note, uint8_t channel) { (void)note; (void)channel; return ESP_OK; }

esp_err_t inter_mcu_send_sample_ctrl(uint8_t slot, wavex_sample_ctrl_cmd_t cmd, float rate) { (void)slot; (void)cmd; (void)rate; return ESP_OK; }

esp_err_t inter_mcu_send_preview_req(uint8_t slot, uint32_t start, uint32_t end, uint16_t decim) { (void)slot; (void)start; (void)end; (void)decim; return ESP_OK; }

void inter_mcu_set_meter_listener(wavex_meter_cb_t cb, void* user_data) { s_meter_cb = cb; s_meter_ud = user_data; }

void inter_mcu_set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data) { s_wave_cb = cb; s_wave_ud = user_data; }

#endif // ESP_PLATFORM