#pragma once

#include <math.h>

#include <cmath>

// Per-group CV calibration (gain/offset per control, plus the exponential
// cutoff-shaping curvature) - shared by every CV backend so calibration data
// has one shape regardless of which physical DAC is behind it.
// See docs/features/analog-voice-board.md §3 "Control mapping".

struct CvCal {
    float vcf_cut_gain = 1.0f, vcf_cut_off = 0.0f;
    float vcf_q_gain = 1.0f, vcf_q_off = 0.0f;
    float vca_gain = 1.0f, vca_off = 0.0f;
    float cutoff_k = 3.0f;
};

inline float CvClamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// Exponential response curve matching an analog VCF's cutoff control law.
inline float CvShapeCutoff(float x, float k) {
    float e = expf(k * x) - 1.0f;
    float d = expf(k) - 1.0f;
    return d > 0.0f ? (e / d) : x;
}
