#include "esp_uart_link.h"

#include "../../shared/config/uart_debug_config.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/uart_protocol/uart_protocol.h"
#include "../comm/packet_router.h"
#include "../inter_mcu.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "../../shared/spi_protocol/sequence_tracker.hpp"
#include "../../shared/uart_protocol/frame_scanner.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace {

using namespace WaveX::UartProtocol;
using WaveX::Comm::PacketRouter;

constexpr char TAG[] = "esp_uart_link";

constexpr size_t RX_TEMP_BUFFER = 256;
// Sized to absorb a full driver-ring drain (see DRIVER_RX_RING below) plus
// leftover partial frames, so drain_driver_rx() can't overflow it in the
// common case (overflow is still handled - oldest bytes dropped, logged).
constexpr size_t RX_PENDING_CAPACITY = WAVEX_ESP_UART_INTER_BUF_SIZE * 4;
constexpr size_t MSG_QUEUE_SIZE = 8;

// Driver ring sizes (review Finding 10). RX: at 2 Mbaud (200 KB/s) a 2KB
// ring holds only ~10ms of inbound while this same task can block ~10ms
// feeding a max frame into the TX ring - razor-thin under full-duplex load;
// 8KB gives ~40ms. TX: 2x the max frame (2058B) so uart_write_bytes copies
// the whole frame into the ring and returns instead of blocking on wire
// drain.
constexpr int DRIVER_RX_RING = WAVEX_ESP_UART_INTER_BUF_SIZE * 4;
constexpr int DRIVER_TX_RING = WAVEX_ESP_UART_INTER_BUF_SIZE * 2;

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
    uint32_t seq_drops = 0;    // duplicate/out-of-order frames dropped by SequenceTracker
    uint32_t seq_resyncs = 0;  // peer-reboot resyncs accepted by SequenceTracker
};

std::atomic<TaskHandle_t> s_uart_task_handle{nullptr};
QueueHandle_t s_uart_event_queue = nullptr;
SemaphoreHandle_t s_uart_mutex = nullptr;

// Guarded by s_uart_mutex on every access below; plain ints rather than
// volatile/atomic because the mutex already provides the ordering guarantee -
// volatile here would be a second, misleading claim of synchronization.
static uart_msg_entry_t s_msg_queue[MSG_QUEUE_SIZE];
static int s_msg_head = 0;
static int s_msg_tail = 0;
static int s_msg_count = 0;

// RX frame extraction: shared scan/consume policy (frame_scanner.hpp,
// review P3.14) over caller-provided storage. Only uart_task touches it.
static uint8_t s_rx_storage[RX_PENDING_CAPACITY];
static WaveX::UartProtocol::FrameScanner s_scanner(s_rx_storage, sizeof(s_rx_storage));

static std::atomic<bool> s_uart_running{false};

// Waking the TX side.
//
// uart_link_send() used to only enqueue, leaving the frame to be picked up
// whenever uart_task's 10 ms event wait next expired - so a note-on could sit
// for most of that before reaching the wire, and a full queue drained one frame
// per tick (~80 ms). For a sequencer that jitter *is* the product.
//
// The task blocks on the driver's event queue, so the cheapest wake is to post
// a marker onto that same queue rather than introduce a second primitive and a
// queue set. The queue is only 20 deep and shared with the driver's own RX
// events, so at most one marker is ever outstanding: this flag is set before
// posting and cleared when the task takes it back off.
// UART_EVENT_MAX is a real enumerator the driver never posts, so it is a
// well-defined marker value. The previous 0x7F was outside the enum's value
// range, where the cast's result is formally unspecified (-Wconversion).
constexpr uart_event_type_t kTxWakeEvent = UART_EVENT_MAX;  // only we post it
static std::atomic<bool> s_tx_wake_pending{false};

static void post_tx_wake() {
    if (!s_uart_event_queue) {
        return;
    }
    if (s_tx_wake_pending.exchange(true)) {
        return;  // a marker is already queued; the task will drain everything
    }
    uart_event_t wake{};
    wake.type = kTxWakeEvent;
    if (xQueueSend(s_uart_event_queue, &wake, 0) != pdTRUE) {
        // Queue full: drop the marker rather than block a caller that may be
        // the UI task. The 10 ms wait still picks the frame up.
        s_tx_wake_pending.store(false);
    }
}
static uint16_t s_next_sequence = 1;
static uart_stats_t s_stats;

// Reboot-aware duplicate/out-of-order gate for the Daisy's RX sequence
// stream (same shared SequenceTracker the SPI path uses). Until 2026-07-05
// the live UART transport dispatched every CRC-valid frame regardless of
// sequence number (code review C4/M4). Only touched from uart_task.
static WaveX::Protocol::SequenceTracker s_rx_seq;

// PacketRouter reference (injected via uart_link_set_packet_router)
static WaveX::Comm::PacketRouter* s_packet_router = nullptr;

// Get the injected PacketRouter reference
static WaveX::Comm::PacketRouter& GetRouter() {
    if (!s_packet_router) {
        // Fallback for tests or uninitialized state - this should not happen in production
        static WaveX::Comm::PacketRouter dummy_router;
        return dummy_router;
    }
    return *s_packet_router;
}

void append_rx_data(const uint8_t* data, size_t len) {
    WaveX::UartProtocol::ScanStats append_stats{};
    size_t dropped = s_scanner.Append(data, len, append_stats);
    if (dropped > 0) {
        s_stats.queue_overflows++;
        UART_LOGW(TAG, "RX buffer overflow - dropped %u oldest bytes", (unsigned)dropped);
    }
}

void process_rx_frames() {
    WaveX::UartProtocol::ScanStats scan{};
    s_scanner.Scan(
        [&](const uint8_t* frame, size_t frame_len) {
            uint8_t msg_type;
            uint8_t flags;
            uint16_t seq;
            uint8_t payload[UART_MAX_PAYLOAD];
            size_t payload_len = sizeof(payload);  // in: capacity, out: bytes copied

            if (!ParseUartPacket(frame, frame_len, msg_type, payload, payload_len, seq, flags)) {
                s_stats.crc_errors++;
                UART_LOGE(TAG, "Failed to parse UART packet (len=%d)", static_cast<int>(frame_len));
                return;
            }
            s_stats.packets_received++;
            UART_LOGI(TAG,
                      "RX %s (0x%02X) len=%d seq=%u flags=0x%02X",
                      WaveX::Protocol::MessageTypeName(msg_type),
                      msg_type,
                      static_cast<int>(payload_len),
                      seq,
                      flags);

            // Sequence gate (mirrors daisy_uart_link.cpp): evaluate every
            // frame so the tracker stays in step with the peer's counter;
            // drop duplicates and severe out-of-order, resync-and-continue
            // on the peer-reboot signature instead of wedging.
            const auto seq_result = s_rx_seq.Evaluate(seq);
            if (seq_result == WaveX::Protocol::SequenceTracker::Result::Duplicate ||
                seq_result == WaveX::Protocol::SequenceTracker::Result::OutOfOrder) {
                s_stats.seq_drops++;
                UART_LOGW(TAG,
                          "RX seq=%u dropped (%s), expected=%u",
                          seq,
                          seq_result == WaveX::Protocol::SequenceTracker::Result::Duplicate
                              ? "duplicate"
                              : "out-of-order",
                          s_rx_seq.ExpectedSeq());
                return;
            }
            if (seq_result == WaveX::Protocol::SequenceTracker::Result::ResyncAccept) {
                s_stats.seq_resyncs++;
                UART_LOGW(TAG, "peer reboot detected - seq resynced to %u", seq);
            }

            GetRouter().route_uart_message(
                msg_type, payload_len ? payload : nullptr, payload_len, flags, seq);
        },
        scan);

    s_stats.crc_errors += scan.crc_errors;
    s_stats.frame_sync_errors += scan.sync_errors;
}

bool dequeue_tx_entry(uart_msg_entry_t& out_entry) {
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
        UART_LOGI(TAG,
                  "TX dequeued: seq=%u frame_len=%u s_msg_count=%d",
                  out_entry.seq,
                  out_entry.frame_len,
                  s_msg_count);
    }

    xSemaphoreGive(s_uart_mutex);
    return has_entry;
}

// Drain everything currently in the driver ring into the scanner. Events
// only fire on NEW rx activity: bytes left in the ring after a partial read
// generate no further UART_DATA event until more data arrives, so anything
// not drained here would sit unread indefinitely (review Finding 9).
void drain_driver_rx(uint8_t* temp) {
    int read;
    while ((read = uart_read_bytes(WAVEX_ESP_UART_INTER_NUM, temp, RX_TEMP_BUFFER, 0)) > 0) {
        append_rx_data(temp, static_cast<size_t>(read));
    }
}

void uart_task(void* /*param*/) {
    UART_LOGI(TAG, "UART task started");
    uart_event_t event;
    uint8_t temp[RX_TEMP_BUFFER];
    uint32_t last_event_time = xTaskGetTickCount();
    uint32_t event_count = 0;

    while (s_uart_running) {
        if (xQueueReceive(s_uart_event_queue, &event, pdMS_TO_TICKS(10))) {
            uint32_t now = xTaskGetTickCount();
            event_count++;
            UART_LOGI(TAG,
                      "UART event[%u] type=%d at_tick=%u gap_ms=%u",
                      event_count,
                      event.type,
                      now,
                      (now - last_event_time));
            last_event_time = now;

            if (event.type == kTxWakeEvent) {
                // Our own marker: a frame was queued for transmit. No work
                // needed here - the TX drain below runs every pass - but
                // clearing the flag lets the next send post a new marker.
                // Handled before the switch to keep the marker visibly
                // separate from the events the driver actually posts.
                s_tx_wake_pending.store(false);
            } else {
                switch (event.type) {
                    case UART_DATA: {
                        UART_LOGI(TAG, "UART_DATA event size=%d", (int)event.size);
                        // Drain the whole ring, not just up to event.size /
                        // RX_TEMP_BUFFER bytes of it (review Finding 9).
                        drain_driver_rx(temp);
                        process_rx_frames();
                        break;
                    }
                    case UART_FIFO_OVF:
                    case UART_BUFFER_FULL:
                        UART_LOGE(
                            TAG, "UART overflow (%d), flushing", static_cast<int>(event.type));
                        uart_flush_input(WAVEX_ESP_UART_INTER_NUM);
                        xQueueReset(s_uart_event_queue);
                        s_scanner.Clear();
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
            }
        } else {
            // Periodic processing even without events. Also drain the
            // driver ring: leftover bytes here generate no new UART_DATA
            // event until MORE data arrives, so without this a frame tail
            // could sit unread indefinitely (review Finding 9).
            uint32_t now = xTaskGetTickCount();
            if (now - last_event_time > 5000) {  // No event for 5 seconds
                UART_LOGI(TAG,
                          "No UART events for %u ms (gap_ms=%u)",
                          (unsigned)(now - last_event_time),
                          (unsigned)(now - last_event_time));
                last_event_time = now;  // Reset to avoid spamming
            }
            drain_driver_rx(temp);
            process_rx_frames();
        }

        // Drain everything queued, not one frame per pass. One-per-pass meant
        // a backlog left the wire idle for 10 ms between frames; the bound is
        // the queue's own capacity, so a flooding producer cannot livelock
        // this loop and starve RX.
        uart_msg_entry_t entry;
        for (size_t sent = 0; sent < MSG_QUEUE_SIZE && dequeue_tx_entry(entry); ++sent) {
            UART_LOGI(TAG, "TX: About to write %u bytes to UART", (unsigned)entry.frame_len);
            int written = uart_write_bytes(WAVEX_ESP_UART_INTER_NUM,
                                           reinterpret_cast<const char*>(entry.frame),
                                           entry.frame_len);
            UART_LOGI(TAG,
                      "TX: uart_write_bytes returned %d (expected %u)",
                      written,
                      (unsigned)entry.frame_len);
            if (written != static_cast<int>(entry.frame_len)) {
                UART_LOGE(TAG,
                          "UART write truncated (%d/%d)",
                          written,
                          static_cast<int>(entry.frame_len));
            } else {
                s_stats.packets_sent++;
                UART_LOGI(TAG, "TX: Packet sent successfully (seq=%u)", entry.seq);
            }
            // 15ms > a max frame's 10.29ms wire time at 2 Mbaud (the old
            // 10ms could legitimately expire mid-frame - harmless here
            // since data stays queued, but the timeout was meaningless).
            esp_err_t txw = uart_wait_tx_done(WAVEX_ESP_UART_INTER_NUM, pdMS_TO_TICKS(15));
            if (txw != ESP_OK) {
                UART_LOGW(TAG, "uart_wait_tx_done: %d (frame still draining)", (int)txw);
            }
        }
    }

    UART_LOGI(TAG, "UART task stopping");
    // Publish the exit before self-deleting: uart_link_stop() waits on this
    // before deleting the queue and mutex this task blocks on.
    s_uart_task_handle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

// Set PacketRouter reference for dependency injection
void uart_link_set_packet_router(WaveX::Comm::PacketRouter* packet_router) {
    s_packet_router = packet_router;
    // Initialize the packet router with stats callback
    if (s_packet_router) {
        s_packet_router->set_stats_callback(
            [](uint8_t packet_type) { inter_mcu_increment_packet_stat(packet_type); });
    }
}

esp_err_t uart_link_init(void) {
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
        .flags = {},
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

    err = uart_driver_install(
        WAVEX_ESP_UART_INTER_NUM, DRIVER_RX_RING, DRIVER_TX_RING, 20, &s_uart_event_queue, 0);
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
    s_scanner.Clear();
    s_msg_head = 0;
    s_msg_tail = 0;
    s_msg_count = 0;

    // Register statistics callback (reuse router instance)
    GetRouter().set_stats_callback(
        [](uint8_t msg_type) { inter_mcu_increment_packet_stat(msg_type); });

    UART_LOGI(TAG,
              "UART link initialized: UART%d @ %d baud on GPIO%d(TX)/GPIO%d(RX)",
              WAVEX_ESP_UART_INTER_NUM,
              WAVEX_ESP_UART_INTER_BAUD,
              WAVEX_ESP_UART_INTER_TX,
              WAVEX_ESP_UART_INTER_RX);

    return ESP_OK;
}

esp_err_t uart_link_start(void) {
    if (!s_uart_mutex) {
        esp_err_t err = uart_link_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (s_uart_task_handle) {
        return ESP_OK;
    }

    // Increased stack size to 16384 bytes to prevent stack overflow when processing
    // browse response callbacks which allocate large temporary arrays
    TaskHandle_t handle = nullptr;
    BaseType_t rc = xTaskCreate(uart_task, "uart_link", 16384, nullptr, 6, &handle);
    s_uart_task_handle = handle;
    if (rc != pdPASS) {
        UART_LOGE(TAG, "Failed to create UART task");
        s_uart_task_handle = nullptr;
        return ESP_FAIL;
    }

    UART_LOGI(TAG, "UART link started - task created successfully");
    return ESP_OK;
}

int uart_link_send(uint16_t msg_type, const void* payload, uint16_t len) {
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

    if (s_msg_count >= static_cast<int>(MSG_QUEUE_SIZE)) {
        s_stats.queue_overflows++;
        UART_LOGE(TAG, "UART TX queue full (size=%d)", MSG_QUEUE_SIZE);
        xSemaphoreGive(s_uart_mutex);
        return -1;
    }

    uart_msg_entry_t& entry = s_msg_queue[s_msg_tail];
    uint16_t seq = s_next_sequence++;
    if (seq == 0) {  // 0 is reserved (receivers reject it); skip it on uint16 wrap
        seq = s_next_sequence++;
    }
    size_t frame_len = CreateUartPacket(
        entry.frame, sizeof(entry.frame), static_cast<uint8_t>(msg_type), payload, len, seq, 0);
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

    // Outside the mutex: post_tx_wake() touches only the event queue, and the
    // task it wakes will want this mutex immediately.
    post_tx_wake();

    UART_LOGI(TAG,
              "TX queued %s (0x%02X) len=%u seq=%u",
              WaveX::Protocol::MessageTypeName(msg_type),
              msg_type,
              len,
              seq);
    UART_LOG_DUMP_PACKET(TAG, entry.frame, frame_len);

    return len;
}

esp_err_t uart_link_stop(void) {
    s_uart_running = false;

    // Wait for the task to leave its loop and self-delete.
    //
    // The previous version sent a task notification and slept 20 ms. Neither
    // did what it looked like: the task blocks in xQueueReceive(), which a
    // notification does not wake, and the fixed sleep was a guess rather than
    // a handshake. It then cleared the handle and deleted the queue, mutex and
    // driver the task could still be parked on - deleting a FreeRTOS object a
    // task is blocked on is undefined behaviour.
    //
    // The loop's own 10 ms event-wait timeout bounds how long it can take to
    // notice; the allowance below covers a pass that is mid-transmit.
    for (int waited_ms = 0; s_uart_task_handle && waited_ms < 300; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_uart_task_handle) {
        // Freeing what it is blocked on would be worse than leaking it.
        UART_LOGE(TAG, "UART task did not exit; leaving the link resources allocated");
        return ESP_ERR_TIMEOUT;
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

void uart_link_log_stats(void) {
    UART_LOGI(TAG,
              "UART stats: sent=%u received=%u crc_errors=%u sync_errors=%u overflow=%u "
              "seq_drops=%u resyncs=%u",
              s_stats.packets_sent,
              s_stats.packets_received,
              s_stats.crc_errors,
              s_stats.frame_sync_errors,
              s_stats.queue_overflows,
              s_stats.seq_drops,
              s_stats.seq_resyncs);
}
