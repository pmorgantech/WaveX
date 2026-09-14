#pragma once

// tan(pi * f) for a normalised frequency f = cutoff / sample_rate, the one
// transcendental every TPT filter here pays on retune (svf_filter.hpp's g,
// ladder_zdf.hpp's g).
//
// Below a quarter of the sample rate it is the odd polynomial Mutable
// Instruments' stmlib (MIT, Emilie Gillet) ships as FREQUENCY_FAST: its two
// coefficients are fitted rather than the Taylor 1/3 and 2/15, and at 48 kHz
// it is within 0.04% of tan up to 8 kHz and 0.25% up to 12 kHz
// (fast_tan_test.cpp pins both). Above that the polynomial diverges quickly
// (5% at 16 kHz, 27% at 20 kHz), so the real tan is used; a retune that high
// is rare and a wrong cutoff there would be audible. Seven multiplies and no
// library call on the common path.
//
// Callers keep their own bypass handling for f >= 0.5; this only promises
// f in [0, 0.5).

#include <cmath>

namespace WaveX {
namespace AudioEngine {

inline float TanPi(float f) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kPolynomialLimit = 0.25f;  // 12 kHz at 48 kHz
    if (f < kPolynomialLimit) {
        constexpr float kPi3 = kPi * kPi * kPi;
        constexpr float kPi5 = kPi3 * kPi * kPi;
        constexpr float a = 3.260e-01f * kPi3;
        constexpr float b = 1.823e-01f * kPi5;
        const float f2 = f * f;
        return f * (kPi + f2 * (a + b * f2));
    }
    return std::tan(kPi * f);
}

}  // namespace AudioEngine
}  // namespace WaveX
