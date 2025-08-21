#include "inter_uart.h"
#include "../config.hpp"
#include <cstring>

using namespace daisy;
using namespace WaveX::Protocol;

namespace WaveX {
namespace Comm {

static DaisySeed* s_hw = nullptr;
static UartHandler s_uart;
static uint8_t s_uart_tx_buffer[256];

// Minimal RX ring buffer for ISR-safe byte capture
static volatile uint8_t  s_rx_ring[256];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;
static volatile uint32_t s_rx_total = 0;

// Data queue for outgoing messages (Daisy → ESP32)
static QueuedMessage s_message_queue[4];
static volatile uint8_t s_queue_head = 0;
static volatile uint8_t s_queue_tail = 0;
static volatile bool s_has_pending_data = false;

// UART debugging counters
static volatile uint32_t s_uart_tx_count = 0;
static volatile uint32_t s_uart_tx_errors = 0;
static volatile uint32_t s_last_uart_activity = 0;

// Simple RX parser state for ProtocolHandler packets (header then payload)
enum class RxParseState : uint8_t { FindSync = 0, ReadHeader = 1, ReadPayload = 2 };
static RxParseState s_rx_state = RxParseState::FindSync;
static uint8_t s_rx_header[sizeof(PacketHeader)];
static uint8_t s_rx_packet[sizeof(PacketHeader) + MAX_PAYLOAD_SIZE];
static uint8_t s_rx_header_pos = 0;
static uint8_t s_rx_payload_pos = 0;
static uint8_t s_expected_payload_len = 0;

static inline bool rx_ring_pop(uint8_t& out)
{
    if(s_rx_head == s_rx_tail) return false;
    out = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1) & 0xFF);
    return true;
}

// UART receive callback for interrupt-driven reception (DMA listen mode)
static void OnUartDataReceived(uint8_t* data, size_t size, void* /*context*/, UartHandler::Result result)
{
    if (result == UartHandler::Result::OK) {
        for (size_t i = 0; i < size; i++) {
            uint16_t next = (s_rx_head + 1) & 0xFF;
            if (next != s_rx_tail) {
                s_rx_ring[s_rx_head] = data[i];
                s_rx_head = next;
                s_rx_total++;
            }
        }
    }
}

static inline void UART_Send_Internal(const uint8_t* data, size_t length) {
    if (s_uart.BlockingTransmit(const_cast<uint8_t*>(data), length, 1000) == UartHandler::Result::OK) {
        s_has_pending_data = false;
        s_uart_tx_count++;
        s_last_uart_activity = System::GetNow();
#if WAVEX_UART_DEBUG_LOG
        if (length >= sizeof(PacketHeader) && data[0] == SYNC_BYTE) {
            const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(data);
            uint8_t msg_type = hdr->type;
            uint8_t payload_len = hdr->length;
            switch (msg_type) {
                case MSG_HEARTBEAT:
                    s_hw->PrintLine("UART TX: HEARTBEAT (%u payload + header + CRC = %u bytes), total=%lu",
                                    payload_len, (unsigned)length, (unsigned long)s_uart_tx_count);
                    break;
                case MSG_METER_PUSH:
                case MSG_WAVE_CHUNK:
                    break;
                default:
                    s_hw->PrintLine("UART TX: type=0x%02X (%u payload + header + CRC = %u bytes), total=%lu",
                                    msg_type, payload_len, (unsigned)length, (unsigned long)s_uart_tx_count);
                    break;
            }
        }
#endif
    } else {
        s_uart_tx_errors++;
        #if WAVEX_UART_DEBUG_LOG
        s_hw->PrintLine("UART TX failed! Total errors: %lu", s_uart_tx_errors);
        #endif
    }
}

void Uart_Send(const uint8_t* data, size_t length) {
    UART_Send_Internal(data, length);
}

void Uart_Init(DaisySeed& hw)
{
    s_hw = &hw;

    UartHandler::Config cfg;
    cfg.periph = UartHandler::Config::Peripheral::USART_1;
    cfg.baudrate = INTER_MCU_UART_BAUD_RATE;
    cfg.stopbits = UartHandler::Config::StopBits::BITS_1;
    cfg.parity = UartHandler::Config::Parity::NONE;
    cfg.mode = UartHandler::Config::Mode::TX_RX;
    cfg.wordlength = UartHandler::Config::WordLength::BITS_8;
    cfg.pin_config.rx = Pin(PORTB, 7);
    cfg.pin_config.tx = Pin(PORTB, 6);
    
    if (s_uart.Init(cfg) != UartHandler::Result::OK) {
        s_hw->PrintLine("UartHandler init failed!");
        return;
    }
    s_hw->PrintLine("UART initialized: TX=D13 (PB6), RX=D14 (PB7), Baud=%lu", (unsigned long)cfg.baudrate);

    std::memset(s_uart_tx_buffer, 0, sizeof(s_uart_tx_buffer));

    uint8_t no_data_payload[] = {0};
    size_t packet_len = ProtocolHandler::CreateGenericPacket(
        s_uart_tx_buffer,
        sizeof(s_uart_tx_buffer),
        MSG_SYNC,
        no_data_payload,
        0);
    if (packet_len > 0) {
        s_hw->PrintLine("TX buffer initialized with sync packet");
    } else {
        s_hw->PrintLine("Failed to create TX buffer sync packet");
    }

#if WAVEX_UART_RX_IRQ_MODE
    static uint8_t dma_rx_buffer[256];
    if (s_uart.DmaListenStart(dma_rx_buffer, sizeof(dma_rx_buffer), OnUartDataReceived, nullptr) == UartHandler::Result::OK) {
        s_hw->PrintLine("UART DMA listen mode started");
    } else {
        s_hw->PrintLine("UART DMA listen mode failed to start");
    }
#else
    s_hw->PrintLine("UART polling mode enabled");
#endif
}

void Uart_QueueMessage(uint8_t type, const void* payload, size_t length)
{
    if(length > MAX_PAYLOAD_SIZE) length = MAX_PAYLOAD_SIZE;
    uint8_t next_head = (s_queue_head + 1) % 4;
    if(next_head == s_queue_tail) {
        s_queue_tail = (s_queue_tail + 1) % 4;
    }
    QueuedMessage& msg = s_message_queue[s_queue_head];
    msg.type = type;
    msg.length = (uint8_t)length;
    if(payload && length) {
        std::memcpy(msg.payload, payload, length);
    }
    msg.valid = true;
    s_queue_head = next_head;
    s_has_pending_data = true;
}

bool Uart_GetNextQueuedMessage(QueuedMessage& msg)
{
    if(s_queue_head == s_queue_tail || !s_has_pending_data) {
        return false;
    }
    msg = s_message_queue[s_queue_tail];
    s_message_queue[s_queue_tail].valid = false;
    s_queue_tail = (s_queue_tail + 1) % 4;
    if(s_queue_head == s_queue_tail) {
        s_has_pending_data = false;
    }
    return true;
}

void Uart_PrepareResponsePacket(const QueuedMessage& msg)
{
    if (!msg.valid) {
        std::memset(s_uart_tx_buffer, 0, sizeof(s_uart_tx_buffer));
        return;
    }

    size_t packet_len = 0;
    switch (msg.type) {
        case MSG_METER_PUSH: {
            MeterPushMessage meter_msg;
            std::memcpy(&meter_msg, msg.payload, sizeof(meter_msg));
            packet_len = ProtocolHandler::CreateMeterPushPacket(
                s_uart_tx_buffer,
                sizeof(s_uart_tx_buffer),
                meter_msg);
            break;
        }
        case MSG_WAVE_CHUNK: {
            WaveChunkMessage wave_msg;
            std::memcpy(&wave_msg, msg.payload, sizeof(wave_msg));
            packet_len = ProtocolHandler::CreateWaveChunkPacket(
                s_uart_tx_buffer,
                sizeof(s_uart_tx_buffer),
                wave_msg,
                msg.payload + sizeof(wave_msg),
                msg.length - sizeof(wave_msg));
            break;
        }
        default: {
            packet_len = ProtocolHandler::CreateGenericPacket(
                s_uart_tx_buffer,
                sizeof(s_uart_tx_buffer),
                msg.type,
                msg.payload,
                msg.length);
            break;
        }
    }

    if (packet_len > 0) {
        UART_Send_Internal(s_uart_tx_buffer, packet_len);
    }
}

void Uart_ProcessRxRing()
{
    uint8_t byte = 0;
    while(s_rx_head != s_rx_tail)
    {
        if(!rx_ring_pop(byte)) {
            break;
        }
        switch(s_rx_state)
        {
            case RxParseState::FindSync:
                if(byte == SYNC_BYTE)
                {
                    s_rx_header_pos = 0;
                    s_rx_payload_pos = 0;
                    s_expected_payload_len = 0;
                    s_rx_state = RxParseState::ReadHeader;
                    s_rx_header[s_rx_header_pos++] = byte;
                }
                break;

            case RxParseState::ReadHeader:
                s_rx_header[s_rx_header_pos++] = byte;
                if(s_rx_header_pos >= sizeof(PacketHeader))
                {
                    std::memcpy(s_rx_packet, s_rx_header, sizeof(PacketHeader));
                    const PacketHeader* hdr = reinterpret_cast<const PacketHeader*>(s_rx_header);
                    s_expected_payload_len = hdr->length;
                    if(s_expected_payload_len > MAX_PAYLOAD_SIZE)
                    {
                        s_rx_state = RxParseState::FindSync;
                        break;
                    }
                    if(s_expected_payload_len == 0)
                    {
                        size_t total_len = sizeof(PacketHeader);
                        if(ProtocolHandler::ValidatePacket(s_rx_packet, total_len))
                        {
                            ProcessUARTMessage(s_rx_packet, total_len);
                        }
                        s_rx_state = RxParseState::FindSync;
                    }
                    else
                    {
                        s_rx_payload_pos = 0;
                        s_rx_state = RxParseState::ReadPayload;
                    }
                }
                break;

            case RxParseState::ReadPayload:
                s_rx_packet[sizeof(PacketHeader) + s_rx_payload_pos] = byte;
                if(++s_rx_payload_pos >= s_expected_payload_len)
                {
                    size_t total_len = sizeof(PacketHeader) + s_expected_payload_len;
                    if(ProtocolHandler::ValidatePacket(s_rx_packet, total_len))
                    {
                        ProcessUARTMessage(s_rx_packet, total_len);
                    }
                    s_rx_state = RxParseState::FindSync;
                }
                break;
        }
    }
}

bool Uart_HasPendingData() { return s_has_pending_data; }
uint32_t Uart_GetRxTotal() { return s_rx_total; }

} // namespace Comm
} // namespace WaveX


