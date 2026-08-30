#include "cv/cv_cal.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

// CvClamp01 and CvShapeCutoff sit directly upstream of lrintf() on the DAC
// write path (mcp4728_backend.hpp), so a non-finite value reaching them is
// undefined behaviour rather than a wrong voltage. Both operate on
// calibration data that arrives over the wire via MSG_CV_CAL_SET, which
// OnCvCalSet stores after a group-index bounds check and no value checks at
// all - so "the float is arbitrary" is the real contract, not a hypothetical.

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// The composed pipeline as mcp4728_backend.hpp:50 applies it: clamp the raw
// control, shape it, apply gain/offset, clamp the result. Tests assert on
// this rather than on CvShapeCutoff alone, because it is the outer clamp that
// the DAC path actually depends on for finiteness.
float ShapedCutoff(float raw, const CvCal& c) {
    return CvClamp01(c.vcf_cut_gain * CvShapeCutoff(CvClamp01(raw), c.cutoff_k) + c.vcf_cut_off);
}

}  // namespace

// --- CvClamp01 -------------------------------------------------------------

// Regression for the 2026-08-29 fix: `x < 0` and `x > 1` are BOTH false for
// NaN, so before the isfinite() guard a NaN fell through the ternary
// unclamped and reached lrintf(). Without this test the defect is invisible -
// every in-range value still behaves correctly.
TEST(CvCalTest, Clamp01MapsNaNToZero) {
    EXPECT_FLOAT_EQ(CvClamp01(kNaN), 0.0f);
}

TEST(CvCalTest, Clamp01MapsInfinitiesToZero) {
    EXPECT_FLOAT_EQ(CvClamp01(kInf), 0.0f);
    EXPECT_FLOAT_EQ(CvClamp01(-kInf), 0.0f);
}

TEST(CvCalTest, Clamp01ClampsOutOfRangeInput) {
    EXPECT_FLOAT_EQ(CvClamp01(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(CvClamp01(-1000.0f), 0.0f);
    EXPECT_FLOAT_EQ(CvClamp01(1.5f), 1.0f);
    EXPECT_FLOAT_EQ(CvClamp01(1000.0f), 1.0f);
}

TEST(CvCalTest, Clamp01PassesInRangeInputThrough) {
    EXPECT_FLOAT_EQ(CvClamp01(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(CvClamp01(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(CvClamp01(0.25f), 0.25f);
    EXPECT_FLOAT_EQ(CvClamp01(0.75f), 0.75f);
}

// --- CvShapeCutoff ---------------------------------------------------------

// The curve must not move the endpoints: 0 and 1 are the control extremes and
// a shaping curve that shifts them changes the usable range of the VCF.
TEST(CvCalTest, ShapeCutoffPreservesEndpoints) {
    for (float k: {0.5f, 1.0f, 3.0f, 8.0f}) {
        EXPECT_NEAR(CvShapeCutoff(0.0f, k), 0.0f, 1e-6f) << "k=" << k;
        EXPECT_NEAR(CvShapeCutoff(1.0f, k), 1.0f, 1e-6f) << "k=" << k;
    }
}

// An exponential control law is only usable if it is monotonic - a
// non-monotonic curve means turning the knob up can lower the cutoff.
TEST(CvCalTest, ShapeCutoffIsMonotonicAcrossTheRange) {
    const float k = CvCal{}.cutoff_k;  // the shipped default
    float prev = CvShapeCutoff(0.0f, k);
    for (int i = 1; i <= 100; ++i) {
        const float x = static_cast<float>(i) / 100.0f;
        const float cur = CvShapeCutoff(x, k);
        EXPECT_GT(cur, prev) << "not increasing at x=" << x;
        prev = cur;
    }
}

// The default curvature must actually curve, otherwise the exponential
// response silently degrades to the linear one it exists to replace.
TEST(CvCalTest, DefaultCurvatureBowsBelowLinear) {
    const float k = CvCal{}.cutoff_k;
    EXPECT_LT(CvShapeCutoff(0.5f, k), 0.5f);
}

// k == 0 makes the denominator exp(0)-1 == 0; the guard returns x rather than
// dividing by zero, so the curve degrades to linear instead of producing inf.
TEST(CvCalTest, ShapeCutoffWithZeroCurvatureIsLinearPassthrough) {
    for (float x: {0.0f, 0.25f, 0.5f, 1.0f}) {
        EXPECT_FLOAT_EQ(CvShapeCutoff(x, 0.0f), x) << "x=" << x;
    }
}

// Negative k drives exp(k)-1 negative, which fails the `d > 0` guard and
// falls back to passthrough. Pinned because a future rewrite of the guard as
// `d != 0` would silently start producing an inverted curve here.
TEST(CvCalTest, ShapeCutoffWithNegativeCurvatureIsPassthrough) {
    EXPECT_FLOAT_EQ(CvShapeCutoff(0.3f, -3.0f), 0.3f);
    EXPECT_FLOAT_EQ(CvShapeCutoff(0.9f, -0.001f), 0.9f);
}

// --- The composed DAC path -------------------------------------------------

// The property that actually matters at the hardware boundary: whatever
// arrives over MSG_CV_CAL_SET, the value handed to lrintf() is finite and in
// [0,1]. This is the generic form of the NaN regression above - it holds the
// line for calibration fields that reach the curve rather than the clamp.
TEST(CvCalTest, ShapedCutoffStaysFiniteAndInRangeForHostileCalibration) {
    const float hostile[] = {kNaN, kInf, -kInf, 0.0f, -0.0f, 1e30f, -1e30f, 1.0f, -1.0f, 3.0f};

    for (float gain: hostile) {
        for (float off: hostile) {
            for (float k: hostile) {
                CvCal c;
                c.vcf_cut_gain = gain;
                c.vcf_cut_off = off;
                c.cutoff_k = k;

                for (float raw: {0.0f, 0.5f, 1.0f, kNaN, 2.0f}) {
                    const float out = ShapedCutoff(raw, c);
                    EXPECT_TRUE(std::isfinite(out))
                        << "non-finite output: gain=" << gain << " off=" << off << " k=" << k
                        << " raw=" << raw;
                    EXPECT_GE(out, 0.0f);
                    EXPECT_LE(out, 1.0f);
                }
            }
        }
    }
}

// A very large curvature overflows expf() to inf in both numerator and
// denominator, and inf/inf is NaN. CvShapeCutoff does not defend against this
// itself - the caller's outer CvClamp01 does. This test documents where the
// defence lives, so that moving the clamp gets caught here rather than on a
// bench with a stuck filter.
TEST(CvCalTest, ShapeCutoffOverflowsToNaNButTheOuterClampContainsIt) {
    const float shaped = CvShapeCutoff(0.5f, 1e6f);
    EXPECT_FALSE(std::isfinite(shaped)) << "expf no longer overflows; revisit this test";

    CvCal c;
    c.cutoff_k = 1e6f;
    EXPECT_FLOAT_EQ(ShapedCutoff(0.5f, c), 0.0f);
}
