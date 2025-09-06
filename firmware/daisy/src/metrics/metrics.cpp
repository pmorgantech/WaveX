#include "metrics.h"
#include <cstdint>

namespace WaveX {
namespace Metrics {

// Generic message counter (not UART-specific)
volatile uint32_t g_message_count = 0;

uint32_t GetMessageCount() {
    return g_message_count;
}

void IncrementMessageCount() {
    g_message_count++;
}

} // namespace Metrics
} // namespace WaveX
