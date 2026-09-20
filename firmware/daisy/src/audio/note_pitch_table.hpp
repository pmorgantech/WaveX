#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace WaveX::AudioEngine {
// Immutable after startup. MIDI/root keys are bytes, so every possible integer
// semitone interval has a fixed slot. Use the same libm expression as the old
// trigger path, once outside audio, without approximating its tuning.
class NotePitchTable {
   public:
    void Init() {
        for (int delta = -255; delta <= 255; ++delta)
            ratios_[static_cast<size_t>(delta + 255)] =
                std::pow(2.f, static_cast<float>(delta) / 12.f);
    }
    float Ratio(uint8_t note, uint8_t root) const {
        return ratios_[static_cast<size_t>(static_cast<int>(note) - root + 255)];
    }

   private:
    std::array<float, 511> ratios_{};
};
}  // namespace WaveX::AudioEngine
