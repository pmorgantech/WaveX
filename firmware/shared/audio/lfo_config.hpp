#pragma once
#include <cstdint>

namespace WaveX::LfoControl {
constexpr float kMinRateHz = .01f, kMaxRateHz = 100.f;
// Append-only wire/WXI identities. Existing Instruments keep their durations.
struct Division {
    const char* label;
    int8_t shift;  // cycles per quarter = 2^shift / divisor
    uint8_t divisor;
};
constexpr Division kDivisions[] = {
    {"Off", 0, 1},
    {"1/16", 2, 1},
    {"1/8", 1, 1},
    {"1/4", 0, 1},
    {"1/2", -1, 1},
    {"1 bar", -2, 1},
    {"2 bars", -3, 1},
    {"4 bars", -4, 1},
    {"3/16", 2, 3},
};
constexpr uint8_t kDivisionCount = sizeof(kDivisions) / sizeof(kDivisions[0]);
// Increasing Rate means faster in both Hz and synchronized modes.
constexpr uint8_t kDurationOrder[] = {7, 6, 5, 4, 3, 8, 2, 1};
constexpr bool ValidDivision(uint8_t id) {
    return id < kDivisionCount;
}
constexpr const char* DivisionLabel(uint8_t id) {
    return ValidDivision(id) ? kDivisions[id].label : "Unknown";
}
// Q32 beat phase -> Q32 cycle phase. Keep power-of-two divisions as shifts.
// Divide before multiplying for the dotted duration to avoid early overflow.
inline uint64_t CyclePhase(uint64_t beats, uint8_t id) {
    if (!id || !ValidDivision(id))
        return 0;
    const auto& division = kDivisions[id];
    if (division.divisor == 3)
        return ((beats / 3) << division.shift) + ((beats % 3) << division.shift) / 3;
    return division.shift >= 0 ? beats << division.shift : beats >> -division.shift;
}
}  // namespace WaveX::LfoControl
