#ifndef WAVEX_UART_PROTOCOL_H
#define WAVEX_UART_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "../spi_protocol/protocol.h"

namespace WaveX {
namespace UartProtocol {

// UART framing constants
static constexpr uint8_t UART_START_BYTE = 0xA5;
static constexpr uint8_t UART_END_BYTE = 0x5A;
static constexpr size_t UART_FRAME_OVERHEAD = 10;  // start + len + flags + type + seq + crc + end
static constexpr size_t UART_MAX_PAYLOAD = 2048;   // payload size limit (matches buffer capacity)

// UART packet flags (upper 4 bits used, lower 4 reserved/adaptive)
static constexpr uint8_t UART_FLAG_ACK = 0x80;       // acknowledgment
static constexpr uint8_t UART_FLAG_NACK = 0x40;      // negative acknowledgment
static constexpr uint8_t UART_FLAG_PRIORITY = 0x20;  // high priority (reserved)
static constexpr uint8_t UART_FLAG_FRAG = 0x10;      // fragmented packet (reserved)

// Packet creation (returns total frame size or 0 on error)
size_t CreateUartPacket(uint8_t* buffer,
                        size_t buffer_size,
                        uint8_t msg_type,
                        const void* payload,
                        size_t payload_size,
                        uint16_t sequence_number,
                        uint8_t flags = 0);

// Packet parsing (returns true on success, false on validation failure)
bool ParseUartPacket(const uint8_t* buffer,
                     size_t buffer_size,
                     uint8_t& msg_type,
                     uint8_t* payload_out,
                     size_t& payload_size,
                     uint16_t& sequence_number,
                     uint8_t& flags);

// Frame validation (start/end markers, length, CRC)
bool ValidateUartFrame(const uint8_t* buffer, size_t buffer_size);

// CRC helper (reuses SPI protocol CRC implementation)
uint16_t CalculateUartCrc(const uint8_t* data, size_t length);

// Blocking-transmit timeout for a frame of `frame_bytes` at `baud`: the
// frame's own wire time (10 bits/byte with start/stop) plus a small
// scheduling margin. A transmit timeout must always exceed the frame's wire
// time - a fixed timeout shorter than the largest frame's wire time makes
// that frame deterministically un-sendable (it times out mid-frame, sprays
// a truncated frame at the peer, and retries forever). That was a live bug:
// a max frame (UART_MAX_PAYLOAD + UART_FRAME_OVERHEAD = 2058 bytes) at
// 2 Mbaud needs 10.29 ms of wire time against the old fixed 10 ms timeout -
// see docs/dma-timing-review-2026-07-03.md, Finding 2.
constexpr uint32_t UartTxTimeoutMs(size_t frame_bytes, uint32_t baud) {
    return static_cast<uint32_t>((static_cast<uint64_t>(frame_bytes) * 10u * 1000u) / baud) + 3u;
}

// Stream helpers for scanning DMA buffers
int FindFrameStart(const uint8_t* buffer, size_t buffer_size);
size_t GetFrameLength(const uint8_t* buffer, size_t buffer_size);

// Debug utility for packet dumps (no-op when logging disabled)
void DumpPacket(const char* tag, const uint8_t* data, size_t length);

}  // namespace UartProtocol
}  // namespace WaveX

#endif  // WAVEX_UART_PROTOCOL_H
