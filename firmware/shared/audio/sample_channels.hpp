#pragma once
#include "spi_protocol/protocol.h"

#include <cstdint>

namespace WaveX::SampleChannels {
// Foreground PCM16 stream mapping. Mono sources feed both stereo outputs;
// selection changes neither stored interleave nor absolute frame positions.
inline void Map(int16_t& left, int16_t& right, uint8_t channels, uint8_t mode) {
    if (channels == 1 || mode == Protocol::SAMPLE_CH_LEFT)
        right = left;
    else if (mode == Protocol::SAMPLE_CH_RIGHT)
        left = right;
    else if (mode == Protocol::SAMPLE_CH_MONO_SUM)
        left = right = static_cast<int16_t>((int32_t(left) + right) / 2);
}
}  // namespace WaveX::SampleChannels
