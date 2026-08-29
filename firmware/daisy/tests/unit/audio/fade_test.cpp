#include "audio/fade.hpp"

#include <gtest/gtest.h>

using WaveX::AudioEngine::FadeFrames;
using WaveX::AudioEngine::FadeGain;
using WaveX::AudioEngine::kMinFadeFrames;
using WaveX::AudioEngine::RegionFadeGain;

// The two properties the whole point rests on: a fade must reach exactly zero
// at the boundary (or it is not removing the step) and exactly unity inside
// (or it is a volume change, not a fade).
TEST(FadeTest, EndpointsAreExactlyZeroAndUnity) {
    EXPECT_FLOAT_EQ(FadeGain(0, 100), 0.0f);
    EXPECT_FLOAT_EQ(FadeGain(100, 100), 1.0f);
    EXPECT_FLOAT_EQ(FadeGain(500, 100), 1.0f);
}

// A length of 0 means "no fade", which has to read as unity. Returning
// silence here would mute every sample with fades turned off.
TEST(FadeTest, AbsentFadeIsUnityNotSilence) {
    EXPECT_FLOAT_EQ(FadeGain(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(FadeGain(50, 0), 1.0f);
}

TEST(FadeTest, RaisedCosineIsMonotonicAndHalfwayAtTheMidpoint) {
    float previous = -1.0f;
    for (uint32_t i = 0; i <= 128; ++i) {
        const float g = FadeGain(i, 128);
        EXPECT_GE(g, previous) << "not monotonic at " << i;
        previous = g;
    }
    EXPECT_NEAR(FadeGain(64, 128), 0.5f, 1e-5f);
}

// The reason for the shape: a linear ramp has a corner at each end, and a
// corner in amplitude is a discontinuity in the first derivative - audible as
// a faint thump on exactly the material a click was the problem on. A raised
// cosine leaves and arrives with zero slope.
TEST(FadeTest, SlopeIsFlatAtBothEnds) {
    const uint32_t len = 1024;
    const float slope_at_start = FadeGain(1, len) - FadeGain(0, len);
    const float slope_mid = FadeGain(len / 2 + 1, len) - FadeGain(len / 2, len);
    const float slope_at_end = FadeGain(len, len) - FadeGain(len - 1, len);

    EXPECT_LT(slope_at_start, slope_mid / 10.0f);
    EXPECT_LT(slope_at_end, slope_mid / 10.0f);
    // A linear ramp would have every slope equal; this must not.
    EXPECT_GT(slope_mid, slope_at_start * 10.0f);
}

TEST(FadeTest, RegionFadeShapesBothEndsAndLeavesTheMiddleAlone) {
    const uint32_t start = 1000;
    const uint32_t end = 3000;
    const uint32_t fade = 100;

    EXPECT_FLOAT_EQ(RegionFadeGain(start, start, end, fade, fade), 0.0f);
    EXPECT_FLOAT_EQ(RegionFadeGain(end - 1, start, end, fade, fade), 0.0f);
    EXPECT_FLOAT_EQ(RegionFadeGain(2000, start, end, fade, fade), 1.0f);
    EXPECT_GT(RegionFadeGain(start + fade / 2, start, end, fade, fade), 0.2f);
    EXPECT_LT(RegionFadeGain(start + fade / 2, start, end, fade, fade), 0.8f);
}

TEST(FadeTest, OneSidedFadesLeaveTheOtherEndAlone) {
    const uint32_t start = 0;
    const uint32_t end = 1000;
    EXPECT_FLOAT_EQ(RegionFadeGain(end - 1, start, end, 100, 0), 1.0f);
    EXPECT_FLOAT_EQ(RegionFadeGain(start, start, end, 0, 100), 1.0f);
}

// Fades that together exceed the region would multiply into audio that never
// reaches unity - which reads as "the sample got quieter", not as a fade.
TEST(FadeTest, OverlongFadesShareTheRegionInsteadOfMultiplying) {
    const uint32_t start = 0;
    const uint32_t end = 200;
    // 1000 frames of fade each way over a 200-frame region.
    for (uint32_t f = start; f < end; ++f) {
        const float g = RegionFadeGain(f, start, end, 1000, 1000);
        EXPECT_GE(g, 0.0f);
        EXPECT_LE(g, 1.0f);
    }
    // The peak sits in the middle and is a real peak, not a permanent dip.
    EXPECT_GT(RegionFadeGain(100, start, end, 1000, 1000), 0.9f);
}

// A ramp of a couple of frames does not reliably remove the step it exists
// for, and a one-frame "fade" is just a smaller step.
TEST(FadeTest, FadesShorterThanTheMinimumAreIgnored) {
    EXPECT_FLOAT_EQ(RegionFadeGain(0, 0, 1000, kMinFadeFrames - 1, 0), 1.0f);
    EXPECT_FLOAT_EQ(RegionFadeGain(0, 0, 1000, kMinFadeFrames, 0), 0.0f);
}

// Frames outside the region return unity: this shapes audio already inside the
// region, and silencing anything else would hide a clamping bug rather than
// expose it.
TEST(FadeTest, FramesOutsideTheRegionAreUntouched) {
    EXPECT_FLOAT_EQ(RegionFadeGain(999, 1000, 2000, 100, 100), 1.0f);
    EXPECT_FLOAT_EQ(RegionFadeGain(2000, 1000, 2000, 100, 100), 1.0f);
    EXPECT_FLOAT_EQ(RegionFadeGain(500, 2000, 1000, 100, 100), 1.0f);  // inverted region
}

TEST(FadeTest, MillisecondsConvertAgainstTheGivenRate) {
    EXPECT_EQ(FadeFrames(1, 48000), 48u);
    EXPECT_EQ(FadeFrames(1, 44100), 44u);
    EXPECT_EQ(FadeFrames(1000, 48000), 48000u);
    EXPECT_EQ(FadeFrames(0, 48000), 0u);
    EXPECT_EQ(FadeFrames(10, 0), 0u);
    // 65535 ms at 48 kHz is 3.1 G frames - must not wrap.
    EXPECT_EQ(FadeFrames(65535, 48000), 3145680u);
}
