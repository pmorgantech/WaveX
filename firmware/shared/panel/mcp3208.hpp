#pragma once
#include <array>
#include <cstdint>
namespace WaveX::Panel {
// Extra leading zero byte gives a 32-bit DMA transfer. CS toggles per channel;
// the result occupies the low nibble of byte 2 and byte 3 (DS21298E section 6.1).
inline std::array<uint8_t, 4> Mcp3208Command(uint8_t channel) {
    return {0,
            static_cast<uint8_t>(6 | ((channel & 7) >> 2)),
            static_cast<uint8_t>((channel & 3) << 6),
            0};
}
inline bool Mcp3208Result(const uint8_t* rx, uint16_t& value) {
    if (rx[2] & 0x10)
        return false;  // ADC's null bit must be low
    value = static_cast<uint16_t>(((rx[2] & 15) << 8) | rx[3]);
    return true;
}
}  // namespace WaveX::Panel
