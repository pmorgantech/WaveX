#pragma once

#include <cstdint>

namespace WaveX {
namespace Wav {

// RAM voices consume interleaved int16_t directly. Streaming audition has a
// separate decoder and may continue accepting PCM24.
constexpr bool IsResidentSampleFormatSupported(uint16_t bits_per_sample, uint16_t channels) {
    return bits_per_sample == 16 && (channels == 1 || channels == 2);
}

}  // namespace Wav
}  // namespace WaveX
