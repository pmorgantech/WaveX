#pragma once

#include "dsp/basic_math_functions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WaveX::AudioEngine {
// Sample metadata is in tenths of a dB. Both resident resolution and streaming
// derive their gain here; -24 dB is attenuation, not a special mute value.
inline float SampleGainLinear(int16_t db_x10) {
    return std::pow(10.f, std::clamp<int16_t>(db_x10, -240, 120) / 200.f);
}
inline int16_t SampleGainQ13(int16_t db_x10) {
    return static_cast<int16_t>(std::lround(SampleGainLinear(db_x10) * 8192.f));
}
// Foreground streaming only. Q13 represents the entire range through +12 dB.
// CMSIS scales q15 PCM by the fraction with a two-bit shift and saturates.
inline void ApplySampleGain(int16_t* pcm, uint32_t samples, int16_t gain_q13) {
    if (samples && gain_q13 != 8192)
        arm_scale_q15(pcm, gain_q13, 2, pcm, samples);
}
}  // namespace WaveX::AudioEngine
