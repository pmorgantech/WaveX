#include "daisy_uart_link.h"

#include "../../shared/config/link_config.h"
#include "../../shared/config/pin_config.h"
#include "../../shared/config/uart_debug_config.h"
#include "../../shared/spi_protocol/protocol.h"
#include "../../shared/uart_protocol/uart_protocol.h"
#include "daisy_inter_mcu_message_handlers.h"
#include "daisy_seed.h"
#include "sys/dma.h"
#include "uart4_dma_transport.h"
#include "util/scopedirqblocker.h"

#include "../../shared/spi_protocol/sequence_tracker.hpp"
#include "../../shared/uart_protocol/frame_scanner.hpp"
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
    uint32_t seq_drops = 0;    // duplicate/out-of-order frames dropped by SequenceTracker
    uint32_t seq_resyncs = 0;  // peer-reboot resyncs accepted by SequenceTracker
};

alignas(32) static DMA_BUFFER_MEM_SECTION uint8_t s_uart_rx_dma[RX_BUFFER_SIZE];
alignas(32) static DMA_BUFFER_MEM_SECTION uint8_t
    s_uart_tx_dma[UART_MAX_PAYLOAD + UART_FRAME_OVERHEAD];
static uint8_t s_rx_pending[RX_PENDING_CAPACITY];
static volatile size_t s_rx_pending_len = 0;

// RX frame extraction: shared scan/consume policy (frame_scanner.hpp,
// review P3.14) over caller-provided storage. Single-context: only the
// main loop touches it.
static uint8_t s_frame_storage[RX_PENDING_CAPACITY];
static WaveX::UartProtocol::FrameScanner s_scanner(s_frame_storage, sizeof(s_frame_storage));

// The four-deep software queue stays in ordinary AXI SRAM. Only the current
// frame is copied to s_uart_tx_dma, keeping async TX DMA cache-safe without
// spending ~8.3 KiB of the fixed 32 KiB D2 DMA pool on queued frames.
static uart_msg_entry_t s_tx_queue[MSG_QUEUE_SIZE];
static int s_tx_head = 0;
static int s_tx_tail = 0;
static int s_tx_count = 0;

static uint16_t s_next_sequence = 1;
static uart_stats_t s_stats;
static bool s_initialized = false;
static bool s_dma_listening = false;

// Reboot-aware duplicate/out-of-order gate for the peer's RX sequence
// stream (shared with the SPI path's implementation - see
// sequence_tracker.hpp). Until 2026-07-05 only the compiled-out SPI path
// had this protection; the live UART transport dispatched every CRC-valid
// frame regardless of sequence number (code review C4/M4).
static WaveX::Protocol::SequenceTracker s_rx_seq;

void append_rx_data_isr(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    // **CRITICAL**: The data pointer points into the UART DMA circular buffer.
    // We must copy it immediately to avoid reading recycled data on the next wraparound.
    // The transport already invalidated these cache lines, so the data is valid.

    if (len > RX_PENDING_CAPACITY) {
        data += (len - RX_PENDING_CAPACITY);
        len = RX_PENDING_CAPACITY;
        s_stats.queue_overflows++;
    }

    if (s_rx_pending_len + len > RX_PENDING_CAPACITY) {
        // Overflow already means at least one frame is lost. Drop the stale
        // pending span and let the scanner resynchronize instead of doing a
        // multi-kilobyte memmove inside the UART DMA interrupt.
        s_rx_pending_len = 0;
        s_stats.frame_sync_errors++;
    }

    // Copy from DMA buffer into our pending buffer
    // This MUST happen before the transport's next callback overwrites the DMA circular buffer.
    std::memcpy(s_rx_pending + s_rx_pending_len, data, len);
    s_rx_pending_len += len;
}

void pull_pending_into_frame_buffer() {
    daisy::ScopedIrqBlocker lock;
    if (s_rx_pending_len == 0) {
        return;
    }
    WaveX::UartProtocol::ScanStats append_stats{};
    if (s_scanner.Append(s_rx_pending, s_rx_pending_len, append_stats) > 0) {
        s_stats.queue_overflows++;
    }
    s_rx_pending_len = 0;
}

void process_rx_frames() {
    pull_pending_into_frame_buffer();

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
                UART_LOGE("daisy_uart",
                          "Failed to parse UART packet len=%d",
                          static_cast<int>(frame_len));
                return;
            }
            s_stats.packets_received++;
            UART_LOGI("daisy_uart",
                      "RX msg=0x%02X len=%d seq=%u flags=0x%02X",
                      msg_type,
                      static_cast<int>(payload_len),
                      seq,
                      flags);
            UART_LOG_DUMP_PACKET("daisy_uart", frame, frame_len);

            // Sequence gate: every frame from the peer shares one sequence
            // counter, so evaluate all of them (ACK/NACK included) to keep
            // the tracker in step; only Accept/ResyncAccept dispatch.
            // Duplicates and severe out-of-order are dropped; a peer reboot
            // (low, fresh-looking seq after real progress) resyncs and
            // continues instead of wedging.
            const auto seq_result = s_rx_seq.Evaluate(seq);
            if (seq_result == WaveX::Protocol::SequenceTracker::Result::Duplicate ||
                seq_result == WaveX::Protocol::SequenceTracker::Result::OutOfOrder) {
                s_stats.seq_drops++;
                UART_LOGW("daisy_uart",
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
                UART_LOGW("daisy_uart", "peer reboot detected - seq resynced to %u", seq);
                if (s_hw)
                    s_hw->PrintLine("DAISY: peer reboot detected - seq resynced to %u", seq);
            }

            if (flags & UART_FLAG_ACK) {
                UART_LOGI("daisy_uart", "ACK received for msg=0x%02X seq=%u", msg_type, seq);
            } else if (flags & UART_FLAG_NACK) {
                UART_LOGE("daisy_uart", "NACK received for msg=0x%02X seq=%u", msg_type, seq);
            } else {
                ProcessInterMcuMessage(msg_type, seq, payload, payload_len);
            }
        },
        scan);

    s_stats.crc_errors += scan.crc_errors;
    s_stats.frame_sync_errors += scan.sync_errors;
}

void process_tx_queue() {
    // Producer and queue consumer remain main-loop-only. The DMA ISR touches
    // only the transport's completion flag, so it never logs, allocates, or
    // mutates queue ownership.
    static uint32_t first_fail_ms = 0;

    // In-flight gate FIRST. The old order polled TakeTransmitResult() and
    // then checked IsTransmitting(); a TX completing between those two calls
    // left its success result pending and the head entry un-retired, and the
    // send below then put an EXACT duplicate of the frame on the wire (the
    // ESP32's steady "RX seq=N dropped (duplicate), expected=N+1" warnings)
    // while StartTransmit() zeroed the un-taken result. No error fires
    // anywhere, so this was invisible on the Daisy side. Checked in this
    // order, "not transmitting" guarantees any pending result is final
    // before we decide what to send.
    if (Uart4Dma::IsTransmitting()) {
        return;
    }

    bool tx_success = false;
    if (Uart4Dma::TakeTransmitResult(tx_success)) {
        if (s_tx_count > 0) {
            uart_msg_entry_t& completed = s_tx_queue[s_tx_head];
            if (tx_success) {
                s_stats.packets_sent++;
                UART_LOGI("daisy_uart",
                          "TX DMA complete OK (seq=%u len=%u)",
                          completed.seq,
                          completed.frame_len);
                completed.pending = false;
                s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
                --s_tx_count;
                first_fail_ms = 0;
            } else {
                s_stats.tx_errors++;
                const uint32_t now = daisy::System::GetNow();
                if (first_fail_ms == 0) {
                    first_fail_ms = now;
                } else if (now - first_fail_ms > 1000) {
                    UART_LOGE(
                        "daisy_uart", "TX DMA gave up after 1s - dropping seq=%u", completed.seq);
                    completed.pending = false;
                    s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
                    --s_tx_count;
                    first_fail_ms = 0;
                }
            }
        }
    }

    while (s_tx_count > 0 && !s_tx_queue[s_tx_head].pending) {
        s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
        --s_tx_count;
    }
    if (s_tx_count == 0) {
        return;
    }

    uart_msg_entry_t& entry = s_tx_queue[s_tx_head];

    // Any second start of the same seq is a retransmit. After the reorder
    // above the only legitimate source is the explicit retry-after-failure
    // policy, so this should stay rare and is worth a log line; if the
    // ESP32 still reports duplicate drops while this stays silent, the
    // cause is on its receive side, not here.
    static uint16_t s_last_started_seq = 0;
    if (entry.seq == s_last_started_seq && s_hw) {
        s_hw->PrintLine("DAISY: UART TX resend seq=%u", entry.seq);
    }
    s_last_started_seq = entry.seq;

    std::memcpy(s_uart_tx_dma, entry.frame, entry.frame_len);
    if (!Uart4Dma::StartTransmit(s_uart_tx_dma, entry.frame_len)) {
        // StartTransmit records an asynchronous-style failure result so the
        // same retry/drop policy runs on the next pump.
        if (!Uart4Dma::IsTransmitting()) {
            UART_LOGE("daisy_uart", "TX DMA start failed for seq=%u", entry.seq);
        }
    }
}

void uart_rx_listener(const uint8_t* buffer, size_t size) {
    if (size == 0) {
        return;
    }

    append_rx_data_isr(buffer, size);
}

bool configure_uart() {
    UART_LOGI("daisy_uart", "configure_uart: START");
    const daisy::Pin tx_pin = daisy::DaisySeed::GetPin(WAVEX_DAISY_UART_INTER_TX);
    const daisy::Pin rx_pin = daisy::DaisySeed::GetPin(WAVEX_DAISY_UART_INTER_RX);
    UART_LOGI("daisy_uart",
              "configure_uart: Pins configured TX=D%d RX=D%d, calling Init",
              WAVEX_DAISY_UART_INTER_TX,
              WAVEX_DAISY_UART_INTER_RX);

    const bool ok = Uart4Dma::Init(WAVEX_DAISY_UART_INTER_BAUD, tx_pin, rx_pin);
    if (!ok) {
        UART_LOGE("daisy_uart", "Uart init failed");
    }
    UART_LOGI("daisy_uart", "configure_uart: COMPLETE");
    return ok;
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

    if (!Uart4Dma::StartReceive(s_uart_rx_dma, RX_BUFFER_SIZE, uart_rx_listener)) {
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

    if (!Uart4Dma::StopReceive()) {
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
    s_scanner.Clear();
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
    if (!configure_uart()) {
        if (hw)
            hw->PrintLine("DAISY: UART4 DMA initialization failed");
        return;
    }
    std::memset(&s_stats, 0, sizeof(s_stats));
    s_next_sequence = 1;
    s_rx_pending_len = 0;
    s_scanner.Clear();
    s_tx_head = 0;
    s_tx_tail = 0;
    s_tx_count = 0;
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

    // Single-context invariant: UartLinkSend (producer) and process_tx_queue
    // (consumer) both run exclusively on the main loop - every call site is
    // reachable only from main()'s loop or from message handlers dispatched
    // by UartLinkProcess(), and the UART RX ISR never touches TX state. No
    // lock is needed. This used to hold a ScopedIrqBlocker (global IRQ-off,
    // audio included) across CreateUartPacket - a memcpy plus a bitwise
    // software CRC16 over up to 2058 bytes, ~60-130us of masked interrupts
    // per large send, guarding no actual concurrency (review Finding 4).
    // If a future caller sends from ISR context, synchronization must be
    // reintroduced here AND in process_tx_queue.

    if (s_tx_count >= MSG_QUEUE_SIZE) {
        s_stats.queue_overflows++;
        UART_LOGE("daisy_uart", "TX queue full");
        return -1;
    }

    uart_msg_entry_t& entry = s_tx_queue[s_tx_tail];
    uint16_t seq = s_next_sequence++;
    if (seq == 0) {  // 0 is reserved (receivers reject it); skip it on uint16 wrap
        seq = s_next_sequence++;
    }
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

void UartLinkPumpTx() {
    process_tx_queue();
}

void UartLinkProcess() {
    static uint32_t last_log = 0;
    static uint32_t last_error_recovery = 0;
    static uint32_t consecutive_parse_failures = 0;
    uint32_t now = daisy::System::GetNow();

    process_rx_frames();
    process_tx_queue();

    // ========== UART ERROR DETECTION AND RECOVERY ==========
    // Detect when UART is in a broken state (e.g., after ESP32 reset with garbage data)

    // Check for UART hardware errors
    static uint32_t last_error_check = 0;
    if (now - last_error_check > 1000) {  // Check every 1 second
        uint32_t uart_error = Uart4Dma::TakeError();
        if (uart_error != 0) {
            if (s_hw)
                s_hw->PrintLine("DAISY: UART hardware error detected: 0x%04X - resetting DMA",
                                uart_error);
            UART_LOGE("daisy_uart", "UART hardware error: 0x%04X", uart_error);

            // Reset the DMA listener to recover from error state
            reset_uart_dma_listener();
            last_error_recovery = now;
        } else if (!Uart4Dma::IsReceiving()) {
            if (s_hw)
                s_hw->PrintLine("DAISY: UART RX DMA stopped unexpectedly - restarting");
            reset_uart_dma_listener();
            last_error_recovery = now;
        }
        last_error_check = now;
    }

    // Detect excessive CRC errors (indicates a corrupted data stream, e.g.
    // after the ESP32 resets and sprays garbage). Counted over a real
    // 1-second window: the old code compared against the count from the
    // PREVIOUS UartLinkProcess() call - sub-millisecond apart - so the
    // ">10 errors" threshold was effectively per-main-loop-iteration, far
    // stricter than the "in 1 sec" its log claimed, and slow-drip
    // corruption could never trigger recovery (review Finding 7).
    static uint32_t crc_window_start_ms = 0;
    static uint32_t crc_count_at_window_start = 0;
    if (now - crc_window_start_ms >= 1000) {
        uint32_t errors_this_window = s_stats.crc_errors - crc_count_at_window_start;
        if (errors_this_window > 10 && (now - last_error_recovery > 2000)) {
            if (s_hw)
                s_hw->PrintLine(
                    "DAISY: Excessive CRC errors detected (%u in 1 sec) - resetting DMA",
                    errors_this_window);
            UART_LOGE("daisy_uart",
                      "Excessive CRC errors: %u/sec - resetting DMA listener",
                      errors_this_window);

            reset_uart_dma_listener();
            last_error_recovery = now;
        }
        crc_window_start_ms = now;
        crc_count_at_window_start = s_stats.crc_errors;
    }

    // Detect frame buffer stuck in error state
    // If frame_len is zero repeatedly but DMA is still receiving data (s_rx_pending_len > 0),
    // the parser is unable to extract valid frames
    if (s_scanner.Buffered() == 0 && s_rx_pending_len > 0) {
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

#if WAVEX_MCU_LINK_DEBUG
    if (now - last_log > 1000) {  // Log every 1 second
        if (s_hw)
            s_hw->PrintLine(
                "DAISY: UartLinkProcess() executed (buffered=%u s_tx_count=%d "
                "dma_listening=%d)",
                (unsigned)s_scanner.Buffered(),
                s_tx_count,
                s_dma_listening);
        last_log = now;
    }
#else
    (void)last_log;
#endif
}

void UartLinkLogStats() {
    UART_LOGI("daisy_uart",
              "UART stats: sent=%u received=%u crc=%u sync=%u overflow=%u txerr=%u seqdrop=%u "
              "resync=%u",
              s_stats.packets_sent,
              s_stats.packets_received,
              s_stats.crc_errors,
              s_stats.frame_sync_errors,
              s_stats.queue_overflows,
              s_stats.tx_errors,
              s_stats.seq_drops,
              s_stats.seq_resyncs);
}

}  // namespace Comm
}  // namespace WaveX
