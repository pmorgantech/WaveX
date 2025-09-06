#pragma once

#include <cstdint>

namespace WaveX {
namespace Metrics {

// Generic message counter (not UART-specific)
extern volatile uint32_t g_message_count;

// Get current message count
uint32_t GetMessageCount();

// Increment message count
void IncrementMessageCount();

} // namespace Metrics
} // namespace WaveX
