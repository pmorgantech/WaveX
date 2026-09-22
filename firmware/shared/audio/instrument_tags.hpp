#pragma once
#include <cstdint>
namespace WaveX::InstrumentTags {
inline constexpr const char* kNames[] = {
    "Drum", "Bass", "Lead", "Pad", "Keys", "FX", "Vocal", "Loop"};
inline constexpr uint8_t kCount = 8;
inline constexpr uint8_t Mask(uint8_t index) {
    return index < kCount ? static_cast<uint8_t>(1u << index) : 0;
}
}  // namespace WaveX::InstrumentTags
