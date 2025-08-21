#pragma once

#include "daisy_seed.h"
#include "spi_protocol/protocol.h"
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Comm {

// Small queue for pending messages (Daisy -> ESP32)
struct QueuedMessage {
    uint8_t type;
    uint8_t payload[WaveX::Protocol::MAX_PAYLOAD_SIZE];
    uint8_t length;
    bool valid;
};

// Initialize UART module (stores reference to hw for logging)
void Uart_Init(daisy::DaisySeed& hw);

// Process any bytes accumulated in RX ring (state machine to packets)
void Uart_ProcessRxRing();

// Queue a message for transmission (Daisy -> ESP32)
void Uart_QueueMessage(uint8_t type, const void* payload, size_t length);

// Dequeue next queued message, returns false if none
bool Uart_GetNextQueuedMessage(QueuedMessage& msg);

// Prepare packet into internal TX buffer and send immediately
void Uart_PrepareResponsePacket(const QueuedMessage& msg);

// Send raw bytes immediately (blocking transmit)
void Uart_Send(const uint8_t* data, size_t length);

// Status
bool Uart_HasPendingData();
uint32_t Uart_GetRxTotal();

} // namespace Comm
} // namespace WaveX

// Message router callback implemented elsewhere
void ProcessUARTMessage(const uint8_t* buffer, size_t length);


