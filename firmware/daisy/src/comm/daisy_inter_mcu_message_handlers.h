#pragma once

#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace Comm {

// Unified dispatcher for inter-MCU messages irrespective of transport.
void ProcessInterMcuMessage(uint8_t msg_type,
                            uint16_t sequence_number,
                            const uint8_t* payload,
                            size_t payload_size);

} // namespace Comm
} // namespace WaveX


