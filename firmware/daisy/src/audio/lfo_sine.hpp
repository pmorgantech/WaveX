#pragma once
#include <arm_math.h>
namespace WaveX::AudioEngine {
// Both device and host tests compile the same vendored CMSIS sine kernel.
inline float LfoSine(float radians) {
    return arm_sin_f32(radians);
}
}  // namespace WaveX::AudioEngine
