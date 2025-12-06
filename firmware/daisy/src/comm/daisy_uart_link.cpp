#include "daisy_uart_link.h"

#include "../../shared/config/pin_config.h"
#include "../../shared/config/uart_debug_config.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/uart_protocol/uart_protocol.h"
#include "daisy_inter_mcu_message_handlers.h"
#include "daisy_seed.h"
#include "per/uart.h"
#include "sys/dma.h"
#include "util/scopedirqblocker.h"

#include <algorithm>
#include <cstring>

namespace WaveX {
namespace Comm {

using namespace WaveX::UartProtocol;

// Global hardware instance pointer (used by all comm handlers)
daisy::DaisySeed* s_hw = NULL;

constexpr size_t RX_BUFFER_SIZE = WAVEX_DAISY_UART_INTER_BUF_SIZE;
constexpr size_t RX_PENDING_CAPACITY = RX_BUFFER_SIZE * 2;
constexpr size_t MSG_QUEUE_SIZE = 4;

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
    uint32_t tx_errors = 0;
};

static daisy::UartHandler s_uart;

static DMA_BUFFER_MEM_SECTION uint8_t s_uart_rx_dma[RX_BUFFER_SIZE];
static uint8_t s_rx_pending[RX_PENDING_CAPACITY];
static volatile size_t s_rx_pending_len = 0;

static uint8_t s_frame_buffer[RX_PENDING_CAPACITY];
static size_t s_frame_len = 0;

// TX queue in DMA-safe memory (size reduced to 8 to fit in RAM_D2_DMA)
static DMA_BUFFER_MEM_SECTION uart_msg_entry_t s_tx_queue[MSG_QUEUE_SIZE];
static int s_tx_head = 0;
static int s_tx_tail = 0;
static int s_tx_count = 0;
static uart_msg_entry_t* s_tx_inflight = nullptr;

static uint16_t s_next_sequence = 1;
static uart_stats_t s_stats;
static bool s_initialized = false;
static bool s_dma_listening = false;

void append_rx_data_isr(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    // **CRITICAL**: The data pointer points into libDaisy's DMA circular buffer.
    // We must copy it immediately to avoid reading recycled data on the next wraparound.
    // libDaisy already calls dsy_dma_invalidate_cache_for_buffer, so cache is valid.

    if (len > RX_PENDING_CAPACITY) {
        data += (len - RX_PENDING_CAPACITY);
        len = RX_PENDING_CAPACITY;
        s_stats.queue_overflows++;
    }

    if (s_rx_pending_len + len > RX_PENDING_CAPACITY) {
        size_t overflow = (s_rx_pending_len + len) - RX_PENDING_CAPACITY;
        if (overflow >= s_rx_pending_len) {
            s_rx_pending_len = 0;
        } else {
            std::memmove(s_rx_pending, s_rx_pending + overflow, s_rx_pending_len - overflow);
            s_rx_pending_len -= overflow;
        }
        s_stats.frame_sync_errors++;
    }

    // Copy from DMA buffer into our pending buffer
    // This MUST happen before libDaisy's next callback overwrites the DMA circular buffer
    std::memcpy(s_rx_pending + s_rx_pending_len, data, len);
    s_rx_pending_len += len;
}

void pull_pending_into_frame_buffer() {
    daisy::ScopedIrqBlocker lock;
    if (s_rx_pending_len == 0) {
        return;
    }

    size_t available_space = RX_PENDING_CAPACITY - s_frame_len;
    size_t pending_len = s_rx_pending_len;
    size_t to_copy = std::min<size_t>(available_space, pending_len);
    if (to_copy > 0) {
        std::memcpy(s_frame_buffer + s_frame_len, s_rx_pending, to_copy);
        s_frame_len += to_copy;
    }

    if (to_copy < pending_len) {
        s_stats.queue_overflows++;
    }

    s_rx_pending_len = 0;
}

void consume_frame_bytes(size_t count) {
    if (count == 0) {
        return;
    }

    if (count >= s_frame_len) {
        s_frame_len = 0;
        return;
    }

    size_t remaining = s_frame_len - count;
    std::memmove(s_frame_buffer, s_frame_buffer + count, remaining);
    s_frame_len = remaining;
}

void process_rx_frames() {
    static uint32_t last_log = 0;
    uint32_t now = daisy::System::GetNow();
    bool should_log = (now - last_log > 5000);  // Log every 5 seconds

    pull_pending_into_frame_buffer();

    size_t offset = 0;
    while (s_frame_len - offset >= UART_FRAME_OVERHEAD) {
        int start = FindFrameStart(s_frame_buffer + offset, s_frame_len - offset);
        if (start < 0) {
            // No start byte found; discard processed bytes
            if (should_log && offset > 0) {
                if (s_hw)
                    s_hw->PrintLine("DAISY: No frame start in %u bytes",
                                    (unsigned)(s_frame_len - offset));
                // Dump first 32 bytes for diagnosis
                char hex_buf[128] = {0};
                int hex_pos = 0;
                size_t to_dump = std::min<size_t>(32, s_frame_len - offset);
                for (size_t i = 0; i < to_dump && hex_pos < 120; i++) {
                    hex_pos += snprintf(
                        hex_buf + hex_pos, 120 - hex_pos, "%02X ", s_frame_buffer[offset + i]);
                }
                if (s_hw)
                    s_hw->PrintLine("DAISY: RX buffer: %s", hex_buf);
            }
            consume_frame_bytes(offset);
            if (should_log) {
                last_log = now;
            }
            return;
        }

        offset += static_cast<size_t>(start);
        size_t available = s_frame_len - offset;
        size_t frame_len = GetFrameLength(s_frame_buffer + offset, available);
        if (frame_len == 0 || frame_len > available) {
            if (should_log && frame_len > 0) {
                if (s_hw)
                    s_hw->PrintLine("DAISY: Incomplete frame: frame_len=%u available=%u",
                                    (unsigned)frame_len,
                                    (unsigned)available);
            }
            consume_frame_bytes(offset);
            if (should_log) {
                last_log = now;
            }
            return;
        }

        const uint8_t* frame = s_frame_buffer + offset;
        if (!ValidateUartFrame(frame, frame_len)) {
            UART_LOGE("daisy_uart", "Invalid UART frame len=%d", static_cast<int>(frame_len));
            s_stats.crc_errors++;
            if (should_log) {
                if (s_hw)
                    s_hw->PrintLine("DAISY: CRC/validation FAILED len=%u", (unsigned)frame_len);
            }
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
            UART_LOGI("daisy_uart",
                      "RX msg=0x%02X len=%d seq=%u flags=0x%02X",
                      msg_type,
                      static_cast<int>(payload_len),
                      seq,
                      flags);
            if (s_hw)
                s_hw->PrintLine(
                    "DAISY: RX msg=0x%02X len=%d seq=%u", msg_type, (int)payload_len, seq);
            UART_LOG_DUMP_PACKET("daisy_uart", frame, frame_len);
            if (flags & UART_FLAG_ACK) {
                UART_LOGI("daisy_uart", "ACK received for msg=0x%02X seq=%u", msg_type, seq);
            } else if (flags & UART_FLAG_NACK) {
                UART_LOGE("daisy_uart", "NACK received for msg=0x%02X seq=%u", msg_type, seq);
            } else {
                ProcessInterMcuMessage(msg_type, seq, payload, payload_len);
            }
        } else {
            s_stats.crc_errors++;
            UART_LOGE(
                "daisy_uart", "Failed to parse UART packet len=%d", static_cast<int>(frame_len));
            if (s_hw)
                s_hw->PrintLine("DAISY: Parse FAILED len=%u", (unsigned)frame_len);
        }

        offset += frame_len;
    }

    consume_frame_bytes(offset);
    if (should_log) {
        if (s_hw)
            s_hw->PrintLine("DAISY: process_rx_frames: s_frame_len=%u parsed %u bytes",
                            (unsigned)s_frame_len,
                            (unsigned)offset);
        last_log = now;
    }
}

// Note: uart_tx_complete is no longer used (BlockingTransmit handles cleanup synchronously)

void process_tx_queue() {
    // Check if we have messages to send
    uart_msg_entry_t* entry = nullptr;

    // Static variables for transmission timeout tracking
    static uint32_t tx_start_time = 0;
    static bool tx_timeout_logged = false;
    static uint32_t last_skip_log = 0;

    // Removed verbose TX logging - TX is working fine

    // Check for stuck transmissions (timeout after 500ms)
    {
        daisy::ScopedIrqBlocker lock;
        if (s_tx_inflight) {
            uint32_t now = daisy::System::GetNow();

            if (tx_start_time == 0) {
                // Record when transmission started
                tx_start_time = now;
                tx_timeout_logged = false;
            } else if ((now - tx_start_time) > 500) {
                // Transmission has been inflight for >500ms - likely stuck
                if (!tx_timeout_logged) {
                    UART_LOGE("daisy_uart",
                              "TX TIMEOUT - transmission stuck for 500ms (seq=%u), clearing",
                              s_tx_inflight->seq);
                    tx_timeout_logged = true;
                }

                // After 1 second, forcefully clear the stuck transmission
                if ((now - tx_start_time) > 1000) {
                    UART_LOGE("daisy_uart",
                              "TX FORCEFULLY CLEARED after 1s timeout (seq=%u)",
                              s_tx_inflight->seq);
                    s_tx_inflight->pending = false;
                    s_tx_inflight = nullptr;
                    s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
                    if (s_tx_count > 0) {
                        --s_tx_count;
                    }
                    tx_start_time = 0;
                    s_stats.tx_errors++;
                }
            }
        } else {
            // Reset timeout tracking when no transmission in flight
            tx_start_time = 0;
        }
    }

    {
        daisy::ScopedIrqBlocker lock;
        if (s_tx_count > 0) {
            entry = &s_tx_queue[s_tx_head];
            if (entry->pending) {
                // Only attempt transmission if not already in flight
                if (!s_tx_inflight) {
                    s_tx_inflight = entry;  // Mark as sending
                } else {
                    // Previous transmission still in flight, don't queue another
                    uint32_t now = daisy::System::GetNow();
                    if (now - last_skip_log > 1000) {
                        UART_LOGI("daisy_uart",
                                  "Skipping TX: previous transmission in flight (seq=%u)",
                                  s_tx_inflight->seq);
                        last_skip_log = now;
                    }
                    entry = nullptr;
                }
            } else {
                // Entry not pending, skip it
                s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
                --s_tx_count;
                entry = nullptr;
            }
        }
    }

    if (!entry) {
        return;
    }

    // Log when browse response frame is pulled from queue
    uint32_t tx_pull_time_ms = daisy::System::GetNow();
    if (s_hw) {
        // Check if this is a browse response by examining the frame
        if (entry->frame_len > 4 && entry->frame[4] == WaveX::Protocol::MSG_BROWSE_RESP) {
            s_hw->PrintLine("DAISY: Browse response PULLED from queue: seq=%u, t=%lu ms",
                            entry->seq,
                            (unsigned long)tx_pull_time_ms);
        }
    }

    // Validate frame format before sending
    bool frame_valid = true;
    if (entry->frame_len < 10) {
        if (s_hw)
            s_hw->PrintLine("DAISY: ERROR - Frame too short: %u bytes", (unsigned)entry->frame_len);
        UART_LOGE("daisy_uart", "Frame too short: %u bytes", entry->frame_len);
        frame_valid = false;
    } else if (entry->frame[0] != 0xA5) {
        if (s_hw)
            s_hw->PrintLine("DAISY: ERROR - Frame start byte 0x%02X (expect 0xA5) len=%u",
                            entry->frame[0],
                            (unsigned)entry->frame_len);
        UART_LOGE("daisy_uart", "Frame missing start byte! First byte: 0x%02X", entry->frame[0]);
        // Dump frame for diagnosis
        char hex_buf[128] = {0};
        int hex_pos = 0;
        for (size_t i = 0; i < std::min<size_t>(32, entry->frame_len) && hex_pos < 120; i++) {
            hex_pos += snprintf(hex_buf + hex_pos, 120 - hex_pos, "%02X ", entry->frame[i]);
        }
        if (s_hw)
            s_hw->PrintLine("DAISY: Frame hex: %s", hex_buf);
        frame_valid = false;
    } else if (entry->frame[entry->frame_len - 1] != 0x5A) {
        if (s_hw)
            s_hw->PrintLine("DAISY: ERROR - Frame end byte 0x%02X (expect 0x5A)",
                            entry->frame[entry->frame_len - 1]);
        UART_LOGE("daisy_uart",
                  "Frame missing end byte! Last byte: 0x%02X",
                  entry->frame[entry->frame_len - 1]);
        frame_valid = false;
    }

    if (!frame_valid) {
        s_stats.tx_errors++;
        // Mark entry as done and advance queue
        {
            daisy::ScopedIrqBlocker lock;
            entry->pending = false;
            s_tx_inflight = nullptr;
            s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
            if (s_tx_count > 0) {
                --s_tx_count;
            }
        }
    } else {
        // Use BlockingTransmit with very short timeout (10ms instead of 100ms)
        // DmaListen mode requires BlockingTransmit - DmaTransmit won't work while listening
        // Short timeout prevents blocking the main loop; timeout recovery handles stuck states
        UART_LOGI("daisy_uart",
                  "TX frame: len=%u seq=%u starting blocking transmission",
                  entry->frame_len,
                  entry->seq);
        auto res = s_uart.BlockingTransmit(entry->frame, entry->frame_len, 10);

        if (res == daisy::UartHandler::Result::OK) {
            // Transmission successful - immediately clean up and advance queue
            s_stats.packets_sent++;
            uint32_t tx_complete_time_ms = daisy::System::GetNow();
            UART_LOGI("daisy_uart", "TX complete OK (seq=%u len=%u)", entry->seq, entry->frame_len);
            if (s_hw) {
                s_hw->PrintLine("DAISY: TX OK seq=%u, t=%lu ms", entry->seq, (unsigned long)tx_complete_time_ms);
                // Check if this was a browse response
                if (entry->frame_len > 4 && entry->frame[4] == WaveX::Protocol::MSG_BROWSE_RESP) {
                    s_hw->PrintLine("DAISY: Browse response TX COMPLETE: seq=%u, t=%lu ms",
                                    entry->seq,
                                    (unsigned long)tx_complete_time_ms);
                }
            }

            // Advance queue
            {
                daisy::ScopedIrqBlocker lock;
                entry->pending = false;
                s_tx_inflight = nullptr;
                s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
                if (s_tx_count > 0) {
                    --s_tx_count;
                }
            }
            // Reset timeout tracking on success
            tx_start_time = 0;
            tx_timeout_logged = false;
        } else {
            s_stats.tx_errors++;
            UART_LOGE("daisy_uart", "TX FAILED result=%d", (int)res);
            if (s_hw)
                s_hw->PrintLine("DAISY: TX FAILED result=%d seq=%u", (int)res, entry->seq);

            // Track timeout for stuck transmissions
            uint32_t now = daisy::System::GetNow();
            if (tx_start_time == 0) {
                tx_start_time = now;
                tx_timeout_logged = false;
            } else if ((now - tx_start_time) > 1000 && !tx_timeout_logged) {
                // After 1 second of consecutive failures, forcefully clear
                UART_LOGE("daisy_uart", "TX stuck for 1s - forcefully advancing queue");
                tx_timeout_logged = true;

                {
                    daisy::ScopedIrqBlocker lock;
                    entry->pending = false;
                    s_tx_inflight = nullptr;
                    s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
                    if (s_tx_count > 0) {
                        --s_tx_count;
                    }
                    tx_start_time = 0;
                }
            }
        }
    }
}

void uart_rx_listener(uint8_t* buffer,
                      size_t size,
                      void* /*context*/,
                      daisy::UartHandler::Result result) {
    if (result != daisy::UartHandler::Result::OK || size == 0) {
        if (result != daisy::UartHandler::Result::OK) {
            UART_LOGE("daisy_uart", "uart_rx_listener ERROR result=%d", (int)result);
        }
        return;
    }

    UART_LOGI("daisy_uart", "uart_rx_listener RX %u bytes", (unsigned)size);
    append_rx_data_isr(buffer, size);
}

void configure_uart() {
    UART_LOGI("daisy_uart", "configure_uart: START");
    daisy::UartHandler::Config cfg;
    cfg.periph = daisy::UartHandler::Config::Peripheral::UART_4;
    cfg.mode = daisy::UartHandler::Config::Mode::TX_RX;
    cfg.baudrate = WAVEX_DAISY_UART_INTER_BAUD;
    UART_LOGI("daisy_uart", "configure_uart: Config set, getting pins");
    cfg.pin_config.tx = daisy::DaisySeed::GetPin(WAVEX_DAISY_UART_INTER_TX);
    cfg.pin_config.rx = daisy::DaisySeed::GetPin(WAVEX_DAISY_UART_INTER_RX);
    UART_LOGI("daisy_uart",
              "configure_uart: Pins configured TX=D%d RX=D%d, calling Init",
              WAVEX_DAISY_UART_INTER_TX,
              WAVEX_DAISY_UART_INTER_RX);

    auto res = s_uart.Init(cfg);
    UART_LOGI("daisy_uart", "configure_uart: Init returned %d", (int)res);
    if (res != daisy::UartHandler::Result::OK) {
        UART_LOGE("daisy_uart", "Uart init failed");
    }
    UART_LOGI("daisy_uart", "configure_uart: COMPLETE");
}

void start_dma_listener() {
    if (s_dma_listening) {
        if (s_hw)
            s_hw->PrintLine("DAISY: DMA listener already started");
        return;
    }

    if (s_hw)
        s_hw->PrintLine("DAISY: Attempting to start DMA listener on UART4...");
    if (s_hw)
        s_hw->PrintLine(
            "DAISY: DMA RX buffer at %p, size=%u", s_uart_rx_dma, (unsigned)RX_BUFFER_SIZE);

    // Clear the DMA buffer to remove any stale data
    std::memset(s_uart_rx_dma, 0, RX_BUFFER_SIZE);

    if (s_uart.DmaListenStart(s_uart_rx_dma, RX_BUFFER_SIZE, uart_rx_listener, nullptr) !=
        daisy::UartHandler::Result::OK) {
        if (s_hw)
            s_hw->PrintLine("DAISY: ERROR - Failed to start DMA listener");
        UART_LOGE("daisy_uart", "Failed to start DMA listener");
    } else {
        if (s_hw)
            s_hw->PrintLine("DAISY: SUCCESS - DMA listener started");
        s_dma_listening = true;
        UART_LOGI("daisy_uart", "DMA listener started successfully");
    }
}

void stop_dma_listener() {
    if (!s_dma_listening) {
        return;
    }

    if (s_hw)
        s_hw->PrintLine("DAISY: Stopping DMA listener on UART4...");

    if (s_uart.DmaListenStop() != daisy::UartHandler::Result::OK) {
        if (s_hw)
            s_hw->PrintLine("DAISY: ERROR - Failed to stop DMA listener");
        UART_LOGE("daisy_uart", "Failed to stop DMA listener");
    } else {
        if (s_hw)
            s_hw->PrintLine("DAISY: SUCCESS - DMA listener stopped");
        s_dma_listening = false;
        UART_LOGI("daisy_uart", "DMA listener stopped successfully");
    }

    // Clear buffers after stopping
    std::memset(s_uart_rx_dma, 0, RX_BUFFER_SIZE);
    s_rx_pending_len = 0;
    s_frame_len = 0;
}

void reset_uart_dma_listener() {
    if (s_hw)
        s_hw->PrintLine("DAISY: UART recovery - resetting DMA listener");
    UART_LOGI("daisy_uart", "Resetting UART DMA listener");

    stop_dma_listener();

    // Small delay to ensure hardware is settled
    daisy::System::Delay(10);

    start_dma_listener();
}

}  // namespace Comm
}  // namespace WaveX

namespace WaveX {
namespace Comm {

void UartLinkInit(daisy::DaisySeed* hw) {
    if (s_initialized) {
        UART_LOGI("daisy_uart", "UART link already initialized");
        if (hw) {
            hw->PrintLine("DAISY: UART link already initialized");
        }
        return;
    }

    if (hw) {
        hw->PrintLine("DAISY: UART link init starting (UART4)");
    }
    configure_uart();
    std::memset(&s_stats, 0, sizeof(s_stats));
    s_next_sequence = 1;
    s_rx_pending_len = 0;
    s_frame_len = 0;
    s_tx_head = 0;
    s_tx_tail = 0;
    s_tx_count = 0;
    s_tx_inflight = nullptr;
    s_hw = hw;  // Store the hw pointer

    s_initialized = true;
    UART_LOGI("daisy_uart", "UART link initialized: UART4 @ %d baud", WAVEX_DAISY_UART_INTER_BAUD);
    if (hw) {
        hw->PrintLine("DAISY: UART link initialized: UART4 @ %d baud", WAVEX_DAISY_UART_INTER_BAUD);
    }
}

void UartLinkStart() {
    if (!s_initialized) {
        UART_LOGE("daisy_uart", "UartLinkStart before init");
        if (s_hw)
            s_hw->PrintLine("DAISY: ERROR - UartLinkStart called before init");
        return;
    }
    if (s_hw)
        s_hw->PrintLine("DAISY: UartLinkStart() called - starting DMA listener");
    start_dma_listener();
    if (s_hw)
        s_hw->PrintLine("DAISY: UartLinkStart() complete - s_dma_listening=%d", s_dma_listening);
    UART_LOGI("daisy_uart", "UART link started - DMA listener active");
}

int UartLinkSend(uint16_t msg_type, const void* payload, uint16_t len) {
    if (len > UART_MAX_PAYLOAD) {
        UART_LOGE("daisy_uart", "Payload too large (%u)", len);
        return -1;
    }

    if (!payload && len > 0) {
        UART_LOGE("daisy_uart", "Null payload");
        return -1;
    }

    daisy::ScopedIrqBlocker lock;
    
    if (s_tx_count >= MSG_QUEUE_SIZE) {
        s_stats.queue_overflows++;
        UART_LOGE("daisy_uart", "TX queue full");
        return -1;
    }

    uart_msg_entry_t& entry = s_tx_queue[s_tx_tail];
    uint16_t seq = s_next_sequence++;
    size_t frame_len = CreateUartPacket(
        entry.frame, sizeof(entry.frame), static_cast<uint8_t>(msg_type), payload, len, seq, 0);
    
    if (frame_len == 0) {
        UART_LOGE("daisy_uart", "Failed to create packet");
        return -1;
    }

    entry.frame_len = frame_len;
    entry.seq = seq;
    entry.pending = true;

    s_tx_tail = (s_tx_tail + 1) % MSG_QUEUE_SIZE;
    ++s_tx_count;

    UART_LOGI("daisy_uart", "TX queued msg=0x%02X len=%u seq=%u", msg_type, len, seq);
    UART_LOG_DUMP_PACKET("daisy_uart", entry.frame, frame_len);
    
    return len;
}

void UartLinkProcess() {
    static uint32_t last_log = 0;
    static uint32_t last_error_recovery = 0;
    static uint32_t consecutive_parse_failures = 0;
    static uint32_t last_crc_error_count = 0;
    uint32_t now = daisy::System::GetNow();

    process_rx_frames();
    process_tx_queue();

    // ========== UART ERROR DETECTION AND RECOVERY ==========
    // Detect when UART is in a broken state (e.g., after ESP32 reset with garbage data)

    // Check for UART hardware errors
    static uint32_t last_error_check = 0;
    if (now - last_error_check > 1000) {  // Check every 1 second
        int uart_error = s_uart.CheckError();
        if (uart_error != 0) {
            if (s_hw)
                s_hw->PrintLine("DAISY: UART hardware error detected: 0x%04X - resetting DMA",
                                uart_error);
            UART_LOGE("daisy_uart", "UART hardware error: 0x%04X", uart_error);

            // Reset the DMA listener to recover from error state
            reset_uart_dma_listener();
            last_error_recovery = now;
        }
        last_error_check = now;
    }

    // Detect excessive CRC errors (indicates corrupted data stream)
    // This typically happens after ESP32 resets and sends garbage
    uint32_t current_crc_errors = s_stats.crc_errors;
    if (now - last_error_recovery > 2000) {  // Only check 2 seconds after last recovery
        uint32_t new_crc_errors = current_crc_errors - last_crc_error_count;
        if (new_crc_errors > 10) {  // More than 10 CRC errors in a short period
            if (s_hw)
                s_hw->PrintLine(
                    "DAISY: Excessive CRC errors detected (%u in 1 sec) - resetting DMA",
                    new_crc_errors);
            UART_LOGE(
                "daisy_uart", "Excessive CRC errors: %u - resetting DMA listener", new_crc_errors);

            reset_uart_dma_listener();
            last_error_recovery = now;
        }
        last_crc_error_count = current_crc_errors;
    }

    // Detect frame buffer stuck in error state
    // If frame_len is zero repeatedly but DMA is still receiving data (s_rx_pending_len > 0),
    // the parser is unable to extract valid frames
    if (s_frame_len == 0 && s_rx_pending_len > 0) {
        consecutive_parse_failures++;
        if (consecutive_parse_failures > 50 && (now - last_error_recovery > 2000)) {
            if (s_hw)
                s_hw->PrintLine(
                    "DAISY: Frame buffer stuck in error state (%u failures) - resetting DMA",
                    consecutive_parse_failures);
            UART_LOGE("daisy_uart",
                      "Frame buffer stuck: %u consecutive failures",
                      consecutive_parse_failures);

            reset_uart_dma_listener();
            consecutive_parse_failures = 0;
            last_error_recovery = now;
        }
    } else {
        consecutive_parse_failures = 0;
    }

    if (now - last_log > 1000) {  // Log every 1 second
        if (s_hw)
            s_hw->PrintLine(
                "DAISY: UartLinkProcess() executed (s_rx_pending_len=%u s_tx_count=%d "
                "dma_listening=%d)",
                (unsigned)s_rx_pending_len,
                s_tx_count,
                s_dma_listening);
        last_log = now;
    }
}

void UartLinkLogStats() {
    UART_LOGI("daisy_uart",
              "UART stats: sent=%u received=%u crc=%u sync=%u overflow=%u txerr=%u",
              s_stats.packets_sent,
              s_stats.packets_received,
              s_stats.crc_errors,
              s_stats.frame_sync_errors,
              s_stats.queue_overflows,
              s_stats.tx_errors);
}

}  // namespace Comm
}  // namespace WaveX
