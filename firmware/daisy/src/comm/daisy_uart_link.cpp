#include "daisy_uart_link.h"

#include <algorithm>
#include <cstring>

#include "per/uart.h"
#include "sys/dma.h"
#include "util/scopedirqblocker.h"
#include "daisy_seed.h"

#include "../../shared/uart_protocol/uart_protocol.h"
#include "../../shared/config/uart_debug_config.h"
#include "../../shared/config/pin_config.h"
#include "../../shared/spi_protocol/protocol.h"
#include "daisy_inter_mcu_message_handlers.h"

namespace WaveX {
namespace Comm {

using namespace WaveX::UartProtocol;

constexpr size_t RX_BUFFER_SIZE = WAVEX_DAISY_UART_INTER_BUF_SIZE;
constexpr size_t RX_PENDING_CAPACITY = RX_BUFFER_SIZE * 2;
constexpr size_t MSG_QUEUE_SIZE = 8;

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

void append_rx_data_isr(const uint8_t* data, size_t len)
{
    if (!data || len == 0) {
        return;
    }

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

    std::memcpy(s_rx_pending + s_rx_pending_len, data, len);
    s_rx_pending_len += len;
}

void pull_pending_into_frame_buffer()
{
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

void consume_frame_bytes(size_t count)
{
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

void process_rx_frames()
{
    pull_pending_into_frame_buffer();

    size_t offset = 0;
    while (s_frame_len - offset >= UART_FRAME_OVERHEAD) {
        int start = FindFrameStart(s_frame_buffer + offset, s_frame_len - offset);
        if (start < 0) {
            // No start byte found; discard processed bytes
            consume_frame_bytes(offset);
            return;
        }

        offset += static_cast<size_t>(start);
        size_t available = s_frame_len - offset;
        size_t frame_len = GetFrameLength(s_frame_buffer + offset, available);
        if (frame_len == 0 || frame_len > available) {
            consume_frame_bytes(offset);
            return;
        }

        const uint8_t* frame = s_frame_buffer + offset;
        if (!ValidateUartFrame(frame, frame_len)) {
            UART_LOGE("daisy_uart", "Invalid UART frame len=%d", static_cast<int>(frame_len));
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
            UART_LOGI("daisy_uart", "RX msg=0x%02X len=%d seq=%u flags=0x%02X",
                      msg_type,
                      static_cast<int>(payload_len),
                      seq,
                      flags);
            printf("DAISY: UART RX frame msg=0x%02X len=%d seq=%u flags=0x%02X\n",
                   msg_type,
                   (int)payload_len,
                   seq,
                   flags);
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
            UART_LOGE("daisy_uart", "Failed to parse UART packet len=%d", static_cast<int>(frame_len));
            printf("DAISY: UART RX parse failed len=%d\n", (int)frame_len);
        }

        offset += frame_len;
    }

    consume_frame_bytes(offset);
}

void uart_tx_complete(void* /*ctx*/, daisy::UartHandler::Result result)
{
    daisy::ScopedIrqBlocker lock;
    if (!s_tx_inflight) {
        UART_LOGI("daisy_uart", "TX complete but s_tx_inflight is null (spurious?)");
        return;
    }

    printf("DAISY: UART TX complete callback fired - result=%d seq=%u\n", (int)result, s_tx_inflight->seq);

    if (result == daisy::UartHandler::Result::OK) {
        s_stats.packets_sent++;
        printf("DAISY: UART TX complete OK (seq=%u, sent_total=%u)\n", s_tx_inflight->seq, s_stats.packets_sent);
    } else {
        s_stats.tx_errors++;
        printf("DAISY: UART TX complete ERROR (seq=%u result=%d, tx_errors=%u)\n", s_tx_inflight->seq, (int)result, s_stats.tx_errors);
    }

    s_tx_inflight->pending = false;
    s_tx_inflight = nullptr;
    s_tx_head = (s_tx_head + 1) % MSG_QUEUE_SIZE;
    if (s_tx_count > 0) {
        --s_tx_count;
    }
    
    UART_LOGI("daisy_uart", "TX queue after callback: count=%d head=%d tail=%d", s_tx_count, s_tx_head, s_tx_tail);
}

void process_tx_queue()
{
    // Check if we have messages to send
    uart_msg_entry_t* entry = nullptr;
    
    static uint32_t last_log = 0;
    uint32_t now = daisy::System::GetNow();
    if (now - last_log > 500) {  // Log every 500ms
        printf("DAISY: process_tx_queue: s_tx_count=%d s_tx_inflight=%p head=%d tail=%d\n", 
               s_tx_count, s_tx_inflight, s_tx_head, s_tx_tail);
        last_log = now;
    }
    
    {
        daisy::ScopedIrqBlocker lock;
        if (s_tx_count > 0) {
            entry = &s_tx_queue[s_tx_head];
            if (entry->pending) {
                s_tx_inflight = entry;  // Mark as sending
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

    // Validate frame format before sending
    bool frame_valid = true;
    if (entry->frame_len < 10) {
        printf("DAISY: ERROR - Frame too short: %u bytes\n", (unsigned)entry->frame_len);
        UART_LOGE("daisy_uart", "Frame too short: %u bytes", entry->frame_len);
        frame_valid = false;
    } else if (entry->frame[0] != 0xA5) {
        printf("DAISY: ERROR - Frame start byte 0x%02X (expect 0xA5) len=%u\n", entry->frame[0], (unsigned)entry->frame_len);
        UART_LOGE("daisy_uart", "Frame missing start byte! First byte: 0x%02X", entry->frame[0]);
        // Dump frame for diagnosis
        printf("DAISY: Frame hex: ");
        for (size_t i = 0; i < std::min<size_t>(32, entry->frame_len); i++) {
            printf("%02X ", entry->frame[i]);
        }
        printf("\n");
        frame_valid = false;
    } else if (entry->frame[entry->frame_len - 1] != 0x5A) {
        printf("DAISY: ERROR - Frame end byte 0x%02X (expect 0x5A)\n", entry->frame[entry->frame_len - 1]);
        UART_LOGE("daisy_uart", "Frame missing end byte! Last byte: 0x%02X", entry->frame[entry->frame_len - 1]);
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
        // Temporarily use blocking transmit to diagnose DMA callback issue
        printf("DAISY: Sending via BlockingTransmit: frame_len=%u seq=%u\n", (unsigned)entry->frame_len, entry->seq);
        auto res = s_uart.BlockingTransmit(entry->frame, entry->frame_len, 100);
        printf("DAISY: BlockingTransmit returned: result=%d\n", (int)res);

        if (res == daisy::UartHandler::Result::OK) {
            s_stats.packets_sent++;
            printf("DAISY: TX SUCCESS seq=%u\n", entry->seq);
        } else {
            s_stats.tx_errors++;
            printf("DAISY: TX FAILED result=%d\n", (int)res);
        }
        
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
    }
}

void uart_rx_listener(uint8_t* buffer,
                      size_t size,
                      void* /*context*/,
                      daisy::UartHandler::Result result)
{
    if (result != daisy::UartHandler::Result::OK || size == 0) {
        printf("DAISY: uart_rx_listener ERROR result=%d size=%d\n", (int)result, (int)size);
        return;
    }

    printf("DAISY: uart_rx_listener RX %d bytes\n", (int)size);
    append_rx_data_isr(buffer, size);
}

void configure_uart()
{
    UART_LOGI("daisy_uart", "configure_uart: START");
    daisy::UartHandler::Config cfg;
    cfg.periph = daisy::UartHandler::Config::Peripheral::UART_4;
    cfg.mode = daisy::UartHandler::Config::Mode::TX_RX;
    cfg.baudrate = WAVEX_DAISY_UART_INTER_BAUD;
    UART_LOGI("daisy_uart", "configure_uart: Config set, getting pins");
    cfg.pin_config.tx = daisy::DaisySeed::GetPin(WAVEX_DAISY_UART_INTER_TX);
    cfg.pin_config.rx = daisy::DaisySeed::GetPin(WAVEX_DAISY_UART_INTER_RX);
    UART_LOGI("daisy_uart", "configure_uart: Pins configured TX=D%d RX=D%d, calling Init", 
              WAVEX_DAISY_UART_INTER_TX, WAVEX_DAISY_UART_INTER_RX);

    auto res = s_uart.Init(cfg);
    UART_LOGI("daisy_uart", "configure_uart: Init returned %d", (int)res);
    if (res != daisy::UartHandler::Result::OK) {
        UART_LOGE("daisy_uart", "Uart init failed");
    }
    UART_LOGI("daisy_uart", "configure_uart: COMPLETE");
}

void start_dma_listener()
{
    if (s_dma_listening) {
        printf("DAISY: DMA listener already started\n");
        return;
    }

    printf("DAISY: Attempting to start DMA listener on UART4...\n");
    if (s_uart.DmaListenStart(s_uart_rx_dma,
                              RX_BUFFER_SIZE,
                              uart_rx_listener,
                              nullptr) != daisy::UartHandler::Result::OK) {
        printf("DAISY: ERROR - Failed to start DMA listener\n");
        UART_LOGE("daisy_uart", "Failed to start DMA listener");
    } else {
        printf("DAISY: SUCCESS - DMA listener started, buffer=%p size=%d\n", s_uart_rx_dma, RX_BUFFER_SIZE);
        s_dma_listening = true;
        UART_LOGI("daisy_uart", "DMA listener started successfully");
    }
}

} // namespace Comm
} // namespace WaveX

namespace WaveX {
namespace Comm {

void UartLinkInit(daisy::DaisySeed* hw)
{
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

    s_initialized = true;
    UART_LOGI("daisy_uart", "UART link initialized: UART4 @ %d baud", WAVEX_DAISY_UART_INTER_BAUD);
    if (hw) {
        hw->PrintLine("DAISY: UART link initialized: UART4 @ %d baud", WAVEX_DAISY_UART_INTER_BAUD);
    }
}

void UartLinkStart()
{
    if (!s_initialized) {
        UART_LOGE("daisy_uart", "UartLinkStart before init");
        printf("DAISY: ERROR - UartLinkStart called before init\n");
        return;
    }
    printf("DAISY: UartLinkStart() called - starting DMA listener\n");
    start_dma_listener();
    printf("DAISY: UartLinkStart() complete - s_dma_listening=%d\n", s_dma_listening);
    UART_LOGI("daisy_uart", "UART link started - DMA listener active");
}

int UartLinkSend(uint16_t msg_type, const void* payload, uint16_t len)
{
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
    size_t frame_len = CreateUartPacket(entry.frame,
                                        sizeof(entry.frame),
                                        static_cast<uint8_t>(msg_type),
                                        payload,
                                        len,
                                        seq,
                                        0);
    if (frame_len == 0) {
        UART_LOGE("daisy_uart", "Failed to create packet");
        return -1;
    }

    // Verify frame was created correctly (for debugging DMA issues)
    if (entry.frame[0] != 0xA5) {
        printf("DAISY: WARNING - Frame created without 0xA5! First byte=0x%02X\n", entry.frame[0]);
        UART_LOGE("daisy_uart", "CreateUartPacket produced bad frame: start=0x%02X (expect 0xA5)", entry.frame[0]);
    }
    if (entry.frame[frame_len - 1] != 0x5A) {
        printf("DAISY: WARNING - Frame created without 0x5A! Last byte=0x%02X\n", entry.frame[frame_len - 1]);
        UART_LOGE("daisy_uart", "CreateUartPacket produced bad frame: end=0x%02X (expect 0x5A)", entry.frame[frame_len - 1]);
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

void UartLinkProcess()
{
    process_rx_frames();
    process_tx_queue();
}

void UartLinkLogStats()
{
    UART_LOGI("daisy_uart",
              "UART stats: sent=%u received=%u crc=%u sync=%u overflow=%u txerr=%u",
              s_stats.packets_sent,
              s_stats.packets_received,
              s_stats.crc_errors,
              s_stats.frame_sync_errors,
              s_stats.queue_overflows,
              s_stats.tx_errors);
}

} // namespace Comm
} // namespace WaveX


