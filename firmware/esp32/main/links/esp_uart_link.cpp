#include "esp_uart_link.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "../../shared/uart_protocol/uart_protocol.h"
#include "../../shared/config/uart_debug_config.h"
#include "../comm/packet_router.h"
#include "../inter_mcu.h"

namespace {

using namespace WaveX::UartProtocol;
using WaveX::Comm::PacketRouter;

constexpr char TAG[] = "esp_uart_link";

constexpr size_t RX_TEMP_BUFFER = 256;
constexpr size_t RX_PENDING_CAPACITY = WAVEX_ESP_UART_INTER_BUF_SIZE * 2;
constexpr size_t MSG_QUEUE_SIZE = 16;

struct uart_msg_entry_t {
    uint8_t frame[UART_MAX_PAYLOAD + UART_FRAME_OVERHEAD];
    size_t frame_len = 0;
    uint16_t seq = 0;
    bool pending = false;
};

struct uart_stats_t {
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t crc_errors = 0;
    uint32_t frame_sync_errors = 0;
    uint32_t queue_overflows = 0;
};

TaskHandle_t s_uart_task_handle = nullptr;
QueueHandle_t s_uart_event_queue = nullptr;
SemaphoreHandle_t s_uart_mutex = nullptr;

static uart_msg_entry_t s_msg_queue[MSG_QUEUE_SIZE];
static volatile int s_msg_head = 0;
static volatile int s_msg_tail = 0;
static volatile int s_msg_count = 0;

static uint8_t s_rx_pending[RX_PENDING_CAPACITY];
static size_t s_rx_pending_len = 0;

static volatile bool s_uart_running = false;
static uint16_t s_next_sequence = 1;
static uart_stats_t s_stats;

PacketRouter& GetRouter()
{
    return WaveX::Comm::GetPacketRouter();
}

void append_rx_data(const uint8_t* data, size_t len)
{
    if (!data || len == 0) {
        return;
    }

    if (s_rx_pending_len + len > RX_PENDING_CAPACITY) {
        // Buffer overflowing - log it
        UART_LOGW(TAG, "RX buffer overflow! pending=%u + new=%u > capacity=%u",
                 (unsigned)s_rx_pending_len, (unsigned)len, (unsigned)RX_PENDING_CAPACITY);
        size_t to_drop = (s_rx_pending_len + len) - RX_PENDING_CAPACITY;
        
        // Shift down to make room
        if (to_drop >= s_rx_pending_len) {
            s_rx_pending_len = 0;
        } else {
            std::memmove(s_rx_pending, s_rx_pending + to_drop, s_rx_pending_len - to_drop);
            s_rx_pending_len -= to_drop;
        }
    }

    std::memcpy(s_rx_pending + s_rx_pending_len, data, len);
    s_rx_pending_len += len;
    
    // Log first byte of new data to diagnose alignment
    if (s_rx_pending_len - len == 0) {
        // This is the first data we received
        UART_LOGI(TAG, "First data: starts with 0x%02X (expect 0xA5)", data[0]);
    }
}

void consume_pending_bytes(size_t count)
{
    if (count == 0 || count > s_rx_pending_len) {
        s_rx_pending_len = 0;
        return;
    }

    size_t remaining = s_rx_pending_len - count;
    if (remaining > 0) {
        std::memmove(s_rx_pending, s_rx_pending + count, remaining);
    }
    s_rx_pending_len = remaining;
}

void process_rx_frames()
{
    size_t offset = 0;

    while (s_rx_pending_len - offset >= UART_FRAME_OVERHEAD) {
        int start = FindFrameStart(s_rx_pending + offset, s_rx_pending_len - offset);
        if (start < 0) {
            // No start byte found - dump the problematic data
            UART_LOGW(TAG, "No frame start found in %u bytes", (unsigned)(s_rx_pending_len - offset));
            // Dump first 32 bytes for analysis
            char hex_str[128];
            snprintf(hex_str, sizeof(hex_str), "RX bytes: ");
            for (size_t i = 0; i < std::min<size_t>(32, s_rx_pending_len); i++) {
                snprintf(hex_str + strlen(hex_str), sizeof(hex_str) - strlen(hex_str), "%02X ", s_rx_pending[i]);
            }
            ESP_LOGW(TAG, "%s", hex_str);
            
            size_t guard = std::min<size_t>(s_rx_pending_len, UART_FRAME_OVERHEAD - 1);
            if (guard < s_rx_pending_len) {
                consume_pending_bytes(s_rx_pending_len - guard);
            }
            return;
        }

        offset += static_cast<size_t>(start);

        size_t available = s_rx_pending_len - offset;
        size_t frame_len = GetFrameLength(s_rx_pending + offset, available);
        if (frame_len == 0 || frame_len > available) {
            // Wait for more data
            if (offset > 0) {
                consume_pending_bytes(offset);
            }
            return;
        }

        const uint8_t* frame = s_rx_pending + offset;
        if (!ValidateUartFrame(frame, frame_len)) {
            UART_LOGE(TAG, "Invalid UART frame CRC (len=%d)", static_cast<int>(frame_len));
            s_stats.crc_errors++;
            offset += 1;
            continue;
        }

        uint8_t msg_type;
        uint8_t flags;
        uint16_t seq;
        uint8_t payload[UART_MAX_PAYLOAD];
        size_t payload_len = 0;

        if (ParseUartPacket(frame, frame_len, msg_type, payload, payload_len, seq, flags)) {
            s_stats.packets_received++;
            UART_LOGI(TAG, "RX msg=0x%02X len=%d seq=%u flags=0x%02X", msg_type, static_cast<int>(payload_len), seq, flags);
            GetRouter().route_uart_message(msg_type, payload_len ? payload : nullptr, payload_len, flags, seq);
        } else {
            s_stats.crc_errors++;
            UART_LOGE(TAG, "Failed to parse UART packet (len=%d)", static_cast<int>(frame_len));
        }

        offset += frame_len;
    }

    if (offset > 0) {
        consume_pending_bytes(offset);
    }
}

bool dequeue_tx_entry(uart_msg_entry_t& out_entry)
{
    if (!s_uart_mutex) {
        return false;
    }

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(2)) != pdTRUE) {
        return false;
    }

    bool has_entry = (s_msg_count > 0);
    if (has_entry) {
        uart_msg_entry_t& entry = s_msg_queue[s_msg_head];
        out_entry = entry;
        entry.pending = false;
        s_msg_head = (s_msg_head + 1) % MSG_QUEUE_SIZE;
        s_msg_count = s_msg_count - 1;
        UART_LOGI(TAG, "TX dequeued: seq=%u frame_len=%u s_msg_count=%d", out_entry.seq, out_entry.frame_len, s_msg_count);
    }

    xSemaphoreGive(s_uart_mutex);
    return has_entry;
}

void uart_task(void* /*param*/)
{
    UART_LOGI(TAG, "UART task started");
    uart_event_t event;
    uint8_t temp[RX_TEMP_BUFFER];
    uint32_t last_event_time = xTaskGetTickCount();
    uint32_t event_count = 0;

    while (s_uart_running) {
        if (xQueueReceive(s_uart_event_queue, &event, pdMS_TO_TICKS(10))) {
            uint32_t now = xTaskGetTickCount();
            event_count++;
            UART_LOGI(TAG, "UART event[%u] type=%d at_tick=%u gap_ms=%u", event_count, event.type, now, (now - last_event_time));
            last_event_time = now;

            switch (event.type) {
                case UART_DATA:
                {
                    UART_LOGI(TAG, "UART_DATA event size=%d", (int)event.size);
                    int read = uart_read_bytes(WAVEX_ESP_UART_INTER_NUM, temp, std::min<int>(event.size, RX_TEMP_BUFFER), pdMS_TO_TICKS(5));
                    if (read > 0) {
                        UART_LOGI(TAG, "UART read %d bytes", read);
                        append_rx_data(temp, static_cast<size_t>(read));
                        process_rx_frames();
                    } else {
                        UART_LOGW(TAG, "uart_read_bytes returned %d (event.size=%d)", read, event.size);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    UART_LOGE(TAG, "UART overflow (%d), flushing", static_cast<int>(event.type));
                    uart_flush_input(WAVEX_ESP_UART_INTER_NUM);
                    xQueueReset(s_uart_event_queue);
                    s_rx_pending_len = 0;
                    s_stats.queue_overflows++;
                    break;
                case UART_BREAK:
                    UART_LOGW(TAG, "UART break detected");
                    break;
                case UART_PARITY_ERR:
                    UART_LOGW(TAG, "UART parity error");
                    break;
                case UART_FRAME_ERR:
                    UART_LOGW(TAG, "UART frame error");
                    break;
                default:
                    UART_LOGW(TAG, "Unknown UART event type=%d", event.type);
                    break;
            }
        } else {
            // Periodic processing even without events
            uint32_t now = xTaskGetTickCount();
            if (now - last_event_time > 5000) {  // No event for 5 seconds
                UART_LOGI(TAG, "No UART events for %u ms (gap_ms=%u)", (unsigned)(now - last_event_time), (unsigned)(now - last_event_time));
                last_event_time = now;  // Reset to avoid spamming
            }
            process_rx_frames();
        }

        uart_msg_entry_t entry;
        if (dequeue_tx_entry(entry)) {
            UART_LOGI(TAG, "TX: About to write %u bytes to UART", (unsigned)entry.frame_len);
            int written = uart_write_bytes(WAVEX_ESP_UART_INTER_NUM, reinterpret_cast<const char*>(entry.frame), entry.frame_len);
            UART_LOGI(TAG, "TX: uart_write_bytes returned %d (expected %u)", written, (unsigned)entry.frame_len);
            if (written != static_cast<int>(entry.frame_len)) {
                UART_LOGE(TAG, "UART write truncated (%d/%d)", written, static_cast<int>(entry.frame_len));
            } else {
                s_stats.packets_sent++;
                UART_LOGI(TAG, "TX: Packet sent successfully (seq=%u)", entry.seq);
            }
            uart_wait_tx_done(WAVEX_ESP_UART_INTER_NUM, pdMS_TO_TICKS(10));
        }
    }

    UART_LOGI(TAG, "UART task stopping");
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t uart_link_init(void)
{
    if (s_uart_mutex) {
        return ESP_OK;
    }

    uart_config_t config = {
        .baud_rate = WAVEX_ESP_UART_INTER_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = 0,
    };

    esp_err_t err = uart_param_config(WAVEX_ESP_UART_INTER_NUM, &config);
    if (err != ESP_OK) {
        UART_LOGE(TAG, "uart_param_config failed: %d", err);
        return err;
    }

    err = uart_set_pin(WAVEX_ESP_UART_INTER_NUM,
                       WAVEX_ESP_UART_INTER_TX,
                       WAVEX_ESP_UART_INTER_RX,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        UART_LOGE(TAG, "uart_set_pin failed: %d", err);
        return err;
    }

    const int rx_buffer = WAVEX_ESP_UART_INTER_BUF_SIZE;
    const int tx_buffer = WAVEX_ESP_UART_INTER_BUF_SIZE;
    err = uart_driver_install(WAVEX_ESP_UART_INTER_NUM,
                              rx_buffer,
                              tx_buffer,
                              20,
                              &s_uart_event_queue,
                              0);
    if (err != ESP_OK) {
        UART_LOGE(TAG, "uart_driver_install failed: %d", err);
        return err;
    }

    s_uart_mutex = xSemaphoreCreateMutex();
    if (!s_uart_mutex) {
        UART_LOGE(TAG, "Failed to create UART mutex");
        return ESP_FAIL;
    }

    s_uart_running = true;
    s_stats = uart_stats_t{};
    s_next_sequence = 1;
    s_rx_pending_len = 0;
    s_msg_head = 0;
    s_msg_tail = 0;
    s_msg_count = 0;

    // Register statistics callback (reuse router instance)
    GetRouter().set_stats_callback([](uint8_t msg_type) {
        inter_mcu_increment_packet_stat(msg_type);
    });

    UART_LOGI(TAG, "UART link initialized: UART%d @ %d baud on GPIO%d(TX)/GPIO%d(RX)",
              WAVEX_ESP_UART_INTER_NUM, WAVEX_ESP_UART_INTER_BAUD,
              WAVEX_ESP_UART_INTER_TX, WAVEX_ESP_UART_INTER_RX);

    return ESP_OK;
}

esp_err_t uart_link_start(void)
{
    if (!s_uart_mutex) {
        esp_err_t err = uart_link_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (s_uart_task_handle) {
        return ESP_OK;
    }

    BaseType_t rc = xTaskCreate(uart_task, "uart_link", 8192, nullptr, 6, &s_uart_task_handle);
    if (rc != pdPASS) {
        UART_LOGE(TAG, "Failed to create UART task");
        s_uart_task_handle = nullptr;
        return ESP_FAIL;
    }

    UART_LOGI(TAG, "UART link started - task created successfully");
    return ESP_OK;
}

int uart_link_send(uint16_t msg_type, const void* payload, uint16_t len)
{
    if (len > UART_MAX_PAYLOAD) {
        UART_LOGE(TAG, "uart_link_send: payload too large (%u)", len);
        return -1;
    }

    if (!payload && len > 0) {
        UART_LOGE(TAG, "uart_link_send: null payload");
        return -1;
    }

    if (!s_uart_mutex) {
        UART_LOGE(TAG, "uart_link_send before init");
        return -1;
    }

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        UART_LOGE(TAG, "uart_link_send: mutex timeout");
        return -1;
    }

    if (s_msg_count >= MSG_QUEUE_SIZE) {
        s_stats.queue_overflows++;
        UART_LOGE(TAG, "UART TX queue full (size=%d)", MSG_QUEUE_SIZE);
        xSemaphoreGive(s_uart_mutex);
        return -1;
    }

    uart_msg_entry_t& entry = s_msg_queue[s_msg_tail];
    uint16_t seq = s_next_sequence++;
    size_t frame_len = CreateUartPacket(entry.frame,
                                        sizeof(entry.frame),
                                        static_cast<uint8_t>(msg_type),
                                        payload,
                                        len,
                                        seq,
                                        0);
    if (frame_len == 0) {
        UART_LOGE(TAG, "Failed to create UART packet (msg=0x%02X)", msg_type);
        xSemaphoreGive(s_uart_mutex);
        return -1;
    }

    entry.frame_len = frame_len;
    entry.seq = seq;
    entry.pending = true;

    s_msg_tail = (s_msg_tail + 1) % MSG_QUEUE_SIZE;
    s_msg_count = s_msg_count + 1;

    xSemaphoreGive(s_uart_mutex);

    UART_LOGI(TAG, "TX queued msg=0x%02X len=%u seq=%u", msg_type, len, seq);
    UART_LOG_DUMP_PACKET(TAG, entry.frame, frame_len);

    return len;
}

esp_err_t uart_link_stop(void)
{
    s_uart_running = false;

    if (s_uart_task_handle) {
        // Wake task so it can exit
        xTaskNotifyGive(s_uart_task_handle);
        vTaskDelay(pdMS_TO_TICKS(20));
        s_uart_task_handle = nullptr;
    }

    if (s_uart_event_queue) {
        vQueueDelete(s_uart_event_queue);
        s_uart_event_queue = nullptr;
    }

    if (s_uart_mutex) {
        vSemaphoreDelete(s_uart_mutex);
        s_uart_mutex = nullptr;
    }

    uart_driver_delete(WAVEX_ESP_UART_INTER_NUM);

    return ESP_OK;
}

void uart_link_log_stats(void)
{
    UART_LOGI(TAG, "UART stats: sent=%u received=%u crc_errors=%u sync_errors=%u overflow=%u",
              s_stats.packets_sent,
              s_stats.packets_received,
              s_stats.crc_errors,
              s_stats.frame_sync_errors,
              s_stats.queue_overflows);
}


