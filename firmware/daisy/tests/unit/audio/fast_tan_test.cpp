// Pins the error bound fast_tan.hpp promises, so a coefficient typo or a
// moved crossover shows up here rather than as a detuned filter.

#include "audio/fast_tan.hpp"

#include <gtest/gtest.h>

#include <cmath>

using WaveX::AudioEngine::TanPi;

TEST(FastTanTest, PolynomialBandIsWithinAQuarterPercentOfTan) {
    constexpr float kPi = 3.14159265358979323846f;
    float worst_low = 0.0f, worst_mid = 0.0f;
    for (int hz = 16; hz <= 12000; hz += 2) {
        const float f = static_cast<float>(hz) / 48000.0f;
        const float exact = std::tan(kPi * f);
        const float err = std::fabs(TanPi(f) - exact) / exact;
        if (hz <= 8000)
            worst_low = std::max(worst_low, err);
        worst_mid = std::max(worst_mid, err);
    }
    EXPECT_LT(worst_low, 0.0005f);
    EXPECT_LT(worst_mid, 0.003f);
}

TEST(FastTanTest, AboveTheCrossoverItIsTheRealTan) {
    constexpr float kPi = 3.14159265358979323846f;
    for (float f: {0.25f, 0.3f, 0.4f, 0.45f, 0.499f}) {
        EXPECT_FLOAT_EQ(TanPi(f), std::tan(kPi * f));
    }
    // And the two sides meet closely enough that a sweep through the
    // crossover does not step.
    const float below = TanPi(0.2499f), above = TanPi(0.25f);
    EXPECT_NEAR(below, above, 0.01f * above);
}

TEST(FastTanTest, IsMonotonicAndZeroAtZero) {
    EXPECT_FLOAT_EQ(TanPi(0.0f), 0.0f);
    float last = 0.0f;
    for (int i = 1; i < 2400; ++i) {
        const float v = TanPi(static_cast<float>(i) / 4800.0f);
        ASSERT_GT(v, last) << i;
        last = v;
    }
}
