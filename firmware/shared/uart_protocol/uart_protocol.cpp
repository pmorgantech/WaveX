#include "uart_protocol.h"

#include <cstring>
#include <cstdio>

#include "../config/uart_debug_config.h"

namespace WaveX {
namespace UartProtocol {

using WaveX::Protocol::ProtocolHandler;

namespace {

constexpr size_t kHeaderSize = 1 /*start*/ + 2 /*length*/;
constexpr size_t kBodyFixedSize = 1 /*flags*/ + 1 /*type*/ + 2 /*seq*/ + 2 /*crc*/;

}

uint16_t CalculateUartCrc(const uint8_t* data, size_t length)
{
    if (!data || length == 0) {
        return 0;
    }
    return ProtocolHandler::CalculateWaveXCrc(data, length);
}

size_t CreateUartPacket(uint8_t* buffer,
                        size_t buffer_size,
                        uint8_t msg_type,
                        const void* payload,
                        size_t payload_size,
                        uint16_t sequence_number,
                        uint8_t flags)
{
    if (!buffer) {
        return 0;
    }

    if (payload_size > UART_MAX_PAYLOAD) {
        return 0;
    }

    const size_t body_len = payload_size + (kBodyFixedSize);
    const size_t total_frame = UART_FRAME_OVERHEAD + payload_size;

    if (buffer_size < total_frame) {
        return 0;
    }

    // Length field includes flags, msg_type, seq, payload, crc (but not start/length/end)
    const uint16_t length_field = static_cast<uint16_t>(body_len);

    size_t offset = 0;
    buffer[offset++] = UART_START_BYTE;
    buffer[offset++] = static_cast<uint8_t>(length_field & 0xFF);
    buffer[offset++] = static_cast<uint8_t>((length_field >> 8) & 0xFF);
    buffer[offset++] = flags;
    buffer[offset++] = msg_type;
    buffer[offset++] = static_cast<uint8_t>(sequence_number & 0xFF);
    buffer[offset++] = static_cast<uint8_t>((sequence_number >> 8) & 0xFF);

    if (payload_size > 0 && payload) {
        std::memcpy(&buffer[offset], payload, payload_size);
    }
    offset += payload_size;

    const uint16_t crc = CalculateUartCrc(&buffer[kHeaderSize], body_len - 2);
    buffer[offset++] = static_cast<uint8_t>(crc & 0xFF);
    buffer[offset++] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    buffer[offset++] = UART_END_BYTE;

    return total_frame;
}

bool ValidateUartFrame(const uint8_t* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < UART_FRAME_OVERHEAD) {
        return false;
    }

    if (buffer[0] != UART_START_BYTE) {
        return false;
    }

    const uint16_t length_field = static_cast<uint16_t>(buffer[1]) |
                                   (static_cast<uint16_t>(buffer[2]) << 8);

    if (length_field < kBodyFixedSize) {
        return false;
    }

    const size_t expected_frame = kHeaderSize + length_field + 1; // + end byte
    if (expected_frame != buffer_size) {
        return false;
    }

    if (length_field > UART_MAX_PAYLOAD + kBodyFixedSize) {
        return false;
    }

    if (buffer[expected_frame - 1] != UART_END_BYTE) {
        return false;
    }

    const uint16_t received_crc = static_cast<uint16_t>(buffer[expected_frame - 3]) |
                                  (static_cast<uint16_t>(buffer[expected_frame - 2]) << 8);
    const uint16_t calculated_crc = CalculateUartCrc(&buffer[kHeaderSize], length_field - 2);

    return received_crc == calculated_crc;
}

bool ParseUartPacket(const uint8_t* buffer,
                     size_t buffer_size,
                     uint8_t& msg_type,
                     uint8_t* payload_out,
                     size_t& payload_size,
                     uint16_t& sequence_number,
                     uint8_t& flags)
{
    if (!ValidateUartFrame(buffer, buffer_size)) {
        return false;
    }

    const uint16_t length_field = static_cast<uint16_t>(buffer[1]) |
                                   (static_cast<uint16_t>(buffer[2]) << 8);
    const uint8_t* body = &buffer[kHeaderSize];

    flags = body[0];
    msg_type = body[1];
    sequence_number = static_cast<uint16_t>(body[2]) |
                      (static_cast<uint16_t>(body[3]) << 8);

    const size_t payload_len = length_field - kBodyFixedSize;
    if (payload_len > UART_MAX_PAYLOAD) {
        return false;
    }

    if (payload_len > 0 && payload_out) {
        std::memcpy(payload_out, &body[4], payload_len);
    }

    payload_size = payload_len;

    return true;
}

int FindFrameStart(const uint8_t* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < UART_FRAME_OVERHEAD) {
        return -1;
    }

    for (size_t i = 0; i + UART_FRAME_OVERHEAD <= buffer_size; ++i) {
        if (buffer[i] == UART_START_BYTE) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t GetFrameLength(const uint8_t* buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < UART_FRAME_OVERHEAD) {
        return 0;
    }

    if (buffer[0] != UART_START_BYTE) {
        return 0;
    }

    const uint16_t length_field = static_cast<uint16_t>(buffer[1]) |
                                   (static_cast<uint16_t>(buffer[2]) << 8);

    if (length_field < kBodyFixedSize ||
        length_field > UART_MAX_PAYLOAD + kBodyFixedSize) {
        return 0;
    }

    return kHeaderSize + length_field + 1;
}

void DumpPacket(const char* tag, const uint8_t* data, size_t length)
{
#if WAVEX_UART_DEBUG_LEVEL >= UART_LOG_DUMP
    if (!data || length == 0) {
        UART_LOGI(tag ? tag : "uartpkt", "<empty packet>");
        return;
    }

    constexpr size_t bytes_per_line = 16;
    for (size_t i = 0; i < length; i += bytes_per_line) {
        char line[bytes_per_line * 3 + 16];
        size_t pos = 0;
        pos += std::snprintf(&line[pos], sizeof(line) - pos, "%04zx:", i);
        for (size_t j = 0; j < bytes_per_line && (i + j) < length; ++j) {
            pos += std::snprintf(&line[pos], sizeof(line) - pos, " %02X", data[i + j]);
        }
        UART_LOGI(tag ? tag : "uartpkt", "%s", line);
    }
#else
    (void)tag;
    (void)data;
    (void)length;
#endif
}

} // namespace UartProtocol
} // namespace WaveX


