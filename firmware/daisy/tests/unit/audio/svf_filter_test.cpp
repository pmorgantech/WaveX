// Unit tests for SvfFilter (src/audio/svf_filter.hpp) - the TPT state-variable
// lowpass that replaced the one-pole stand-in so PARAM_FILTER_RESONANCE has a
// digital consumer (features/digital-voice-audition.md stage 1).
//
// The properties pinned here are the ones the voice path actually depends on:
// exact bypass above Nyquist (voice_manager_test's FlatParams relies on it),
// monotone attenuation with frequency, a real resonant peak, and - the reason
// this topology was chosen at all - stability when cutoff and resonance are
// modulated while the filter is running.

#include "audio/svf_filter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using WaveX::AudioEngine::SvfFilter;

namespace {

constexpr uint32_t kSr = 48000;
constexpr float kPi = 3.14159265358979323846f;

// Drives a sine at `hz` through `f` and returns the peak amplitude of the
// steady-state tail, skipping the filter's startup transient.
float SteadyStatePeak(SvfFilter& f, float hz, size_t cycles = 200) {
    const size_t n = static_cast<size_t>(static_cast<float>(kSr) / hz * static_cast<float>(cycles));
    const size_t settle = n / 2;
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float in =
            std::sin(2.0f * kPi * hz * static_cast<float>(i) / static_cast<float>(kSr));
        const float out = f.Process(in);
        if (i >= settle)
            peak = std::max(peak, std::fabs(out));
    }
    return peak;
}

SvfFilter MakeFilter(float cutoff_hz, float res = 0.0f) {
    SvfFilter f;
    f.Init(kSr);
    f.SetResonance(res);
    f.SetCutoff(cutoff_hz);
    f.Reset();
    return f;
}

}  // namespace

// --- Bypass contract -------------------------------------------------------
//
// voice_manager_test's FlatParams sets filter_cutoff_hz = 1e6 and expects the
// samples back untouched. If this ever becomes merely "nearly open", every
// amplitude assertion in that suite drifts.

TEST(SvfFilterTest, AtOrAboveNyquistIsExactBypass) {
    SvfFilter f = MakeFilter(1.0e6f);
    const float in[] = {1.0f, -1.0f, 0.5f, -0.25f, 0.0f, 0.99f};
    for (float x: in) {
        EXPECT_FLOAT_EQ(f.Process(x), x) << "cutoff above Nyquist must pass input through exactly";
    }
}

TEST(SvfFilterTest, ExactlyNyquistIsBypass) {
    SvfFilter f = MakeFilter(static_cast<float>(kSr) * 0.5f);
    EXPECT_FLOAT_EQ(f.Process(0.7f), 0.7f);
}

TEST(SvfFilterTest, BypassIgnoresResonance) {
    // Resonance must not sneak a peak onto a voice that asked for no filter.
    SvfFilter f = MakeFilter(1.0e6f, 1.0f);
    EXPECT_FLOAT_EQ(f.Process(0.3f), 0.3f);
}

// --- Lowpass response ------------------------------------------------------

TEST(SvfFilterTest, PassesWellBelowCutoff) {
    SvfFilter f = MakeFilter(2000.0f);
    // A 100 Hz tone is more than four octaves below cutoff - essentially unity.
    EXPECT_NEAR(SteadyStatePeak(f, 100.0f), 1.0f, 0.05f);
}

TEST(SvfFilterTest, AttenuatesWellAboveCutoff) {
    SvfFilter f = MakeFilter(500.0f);
    EXPECT_LT(SteadyStatePeak(f, 8000.0f), 0.05f)
        << "four octaves above a 2-pole cutoff should be far down";
}

TEST(SvfFilterTest, AttenuationIsMonotoneInFrequency) {
    float prev = 2.0f;
    for (float hz: {200.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f}) {
        SvfFilter f = MakeFilter(1000.0f);
        const float peak = SteadyStatePeak(f, hz);
        EXPECT_LT(peak, prev) << "response should fall monotonically at " << hz << " Hz";
        prev = peak;
    }
}

TEST(SvfFilterTest, TwoPoleIsSteeperThanOnePoleWas) {
    // The one-pole this replaced rolled off 6 dB/oct; a 2-pole gives 12. One
    // octave above cutoff the 2-pole must be clearly further down than the
    // ~-7 dB (0.45) a one-pole would give there.
    SvfFilter f = MakeFilter(1000.0f);
    EXPECT_LT(SteadyStatePeak(f, 2000.0f), 0.40f);
}

// --- Known filter math -----------------------------------------------------
//
// The TPT/bilinear structure matches the analog 2nd-order lowpass prototype
// |H(r)| = 1 / sqrt((1 - r^2)^2 + (r/Q)^2) with the frequency axis prewarped:
// r_eff = tan(pi*f/fs) / tan(pi*fc/fs). g = tan(pi*fc/fs) makes the mapping
// exact AT the cutoff, so these are hand-computable expected values, not
// loose "it attenuates" bounds. Test frequencies divide fs exactly, so the
// sampled sine hits its true crest and SteadyStatePeak measures the real
// amplitude rather than an off-crest sample.

namespace {

float AnalogPrototypeMagnitude(float f_hz, float cutoff_hz, float q) {
    const double r = std::tan(3.14159265358979323846 * f_hz / kSr) /
                     std::tan(3.14159265358979323846 * cutoff_hz / kSr);
    const double a = 1.0 - r * r;
    const double b = r / q;
    return static_cast<float>(1.0 / std::sqrt(a * a + b * b));
}

}  // namespace

// At Q = 0.5 (resonance 0) the response is exactly 1/(1 + r^2): 0.5 at the
// cutoff, 0.2 one octave up, 1/17 two octaves up. A one-pole - or any other
// slope - cannot produce these numbers.
TEST(SvfFilterTest, MagnitudeMatchesAnalogPrototypeAtQHalf) {
    for (float hz: {500.0f, 1000.0f, 2000.0f, 4000.0f}) {
        SvfFilter f = MakeFilter(1000.0f, 0.0f);
        const float expected = AnalogPrototypeMagnitude(hz, 1000.0f, 0.5f);
        EXPECT_NEAR(SteadyStatePeak(f, hz), expected, expected * 0.02f + 0.002f)
            << "at " << hz << " Hz (expected |H| = " << expected << ")";
    }
}

// Peak gain of a 2-pole lowpass driven exactly at cutoff is Q. resonance=1
// maps to kMaxQ, so the resonant peak must measure ~kMaxQ - not merely "more
// than flat" - and the cubic curve puts half travel near +4.5 dB.
TEST(SvfFilterTest, ResonantGainAtCutoffEqualsQ) {
    SvfFilter f = MakeFilter(1000.0f, 1.0f);
    EXPECT_NEAR(SteadyStatePeak(f, 1000.0f), SvfFilter::kMaxQ, SvfFilter::kMaxQ * 0.03f);
    SvfFilter half = MakeFilter(1000.0f, 0.5f);
    const float q_half = SvfFilter::kMinQ + 0.125f * (SvfFilter::kMaxQ - SvfFilter::kMinQ);
    EXPECT_NEAR(SteadyStatePeak(half, 1000.0f), q_half, q_half * 0.05f);
}

TEST(SvfFilterTest, TwentyFourDbPeaksAtQNotQSquared) {
    // The second stage is a fixed Butterworth pair: at the cutoff it costs
    // -3 dB and adds no bump, so the resonant peak of the 24 dB filter is
    // the 12 dB peak times 0.707, not Q times Q.
    SvfFilter f12 = MakeFilter(1000.0f, 1.0f);
    SvfFilter f24 = MakeFilter(1000.0f, 1.0f);
    f24.SetSlope(SvfFilter::Slope::Db24);
    const float p12 = SteadyStatePeak(f12, 1000.0f);
    const float p24 = SteadyStatePeak(f24, 1000.0f);
    EXPECT_NEAR(p24, p12 * 0.7071f, p12 * 0.05f);
}

// --- Resonance -------------------------------------------------------------

TEST(SvfFilterTest, ResonanceProducesPeakAtCutoff) {
    SvfFilter flat = MakeFilter(1000.0f, 0.0f);
    SvfFilter resonant = MakeFilter(1000.0f, 1.0f);
    const float flat_peak = SteadyStatePeak(flat, 1000.0f);
    const float res_peak = SteadyStatePeak(resonant, 1000.0f);
    EXPECT_GT(res_peak, flat_peak * 2.0f)
        << "resonance=1 should lift the response at cutoff well above resonance=0";
}

TEST(SvfFilterTest, ZeroResonanceHasNoPeak) {
    // At Q = 0.5 the response must not exceed unity anywhere - a voice that
    // never asked for resonance must not get a bump (or clip) from one.
    for (float hz: {100.0f, 500.0f, 1000.0f, 2000.0f}) {
        SvfFilter f = MakeFilter(1000.0f, 0.0f);
        EXPECT_LE(SteadyStatePeak(f, hz), 1.02f) << "unexpected gain at " << hz << " Hz";
    }
}

TEST(SvfFilterTest, ResonanceIsClampedToStableRange) {
    // Out-of-range values arrive from a wire parameter; they must clamp, not
    // destabilize. k = 1/Q stays > 0 so the filter cannot self-oscillate.
    for (float res: {-5.0f, 1.5f, 100.0f}) {
        SvfFilter f = MakeFilter(1000.0f, res);
        float last = 0.0f;
        for (int i = 0; i < 20000; ++i)
            last = f.Process(std::sin(2.0f * kPi * 1000.0f * static_cast<float>(i) / kSr));
        EXPECT_TRUE(std::isfinite(last)) << "resonance " << res << " produced a non-finite output";
        EXPECT_LT(std::fabs(last), 50.0f) << "resonance " << res << " ran away";
    }
}

// --- Modulation stability --------------------------------------------------
//
// This is the property the TPT topology was chosen for, and the reason a
// direct-form biquad was rejected: cutoff and resonance move *while the
// filter runs*, which is what stage 1 exists to make possible.

TEST(SvfFilterTest, SweepingCutoffWhileRunningStaysFinite) {
    SvfFilter f = MakeFilter(200.0f, 0.9f);
    float peak = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        // Sweep 200 Hz -> 12 kHz and back, changing every sample - a harsher
        // test than the per-block updates the control tick will actually do.
        const float t = static_cast<float>(i) / 48000.0f;
        f.SetCutoff(200.0f + 11800.0f * (0.5f - 0.5f * std::cos(2.0f * kPi * t)));
        const float out = f.Process(std::sin(2.0f * kPi * 440.0f * t));
        ASSERT_TRUE(std::isfinite(out)) << "non-finite output at sample " << i;
        peak = std::max(peak, std::fabs(out));
    }
    EXPECT_LT(peak, 20.0f) << "a cutoff sweep should not blow the filter up";
}

TEST(SvfFilterTest, SweepingResonanceWhileRunningStaysFinite) {
    SvfFilter f = MakeFilter(1000.0f);
    for (int i = 0; i < 48000; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        f.SetResonance(0.5f - 0.5f * std::cos(2.0f * kPi * 3.0f * t));
        const float out = f.Process(std::sin(2.0f * kPi * 1000.0f * t));
        ASSERT_TRUE(std::isfinite(out)) << "non-finite output at sample " << i;
    }
}

// --- State hygiene ---------------------------------------------------------

TEST(SvfFilterTest, ResetClearsStateButKeepsTuning) {
    SvfFilter f = MakeFilter(500.0f);
    for (int i = 0; i < 1000; ++i)
        f.Process(1.0f);  // charge the integrators with DC
    f.Reset();
    // First sample after Reset must behave as it did from cold, not carry the
    // previous note's DC out as a click.
    SvfFilter fresh = MakeFilter(500.0f);
    EXPECT_FLOAT_EQ(f.Process(0.0f), fresh.Process(0.0f));
}

TEST(SvfFilterTest, SilenceInDecaysToSilenceOut) {
    SvfFilter f = MakeFilter(1000.0f, 1.0f);
    for (int i = 0; i < 1000; ++i)
        f.Process(1.0f);
    float last = 0.0f;
    for (int i = 0; i < 48000; ++i)
        last = f.Process(0.0f);
    EXPECT_LT(std::fabs(last), 1.0e-6f) << "resonant filter should ring down to silence";
}

// --- Slope -------------------------------------------------------------------
//
// 24 dB/oct is the same stage twice, so an octave above cutoff it must roll
// off about twice as many dB as 12 dB/oct, while the passband stays put.

TEST(SvfFilterTest, TwentyFourDbRollsOffTwiceAsSteeply) {
    SvfFilter f12 = MakeFilter(1000.0f);
    SvfFilter f24 = MakeFilter(1000.0f);
    f24.SetSlope(SvfFilter::Slope::Db24);

    const float pass12 = SteadyStatePeak(f12, 50.0f);
    const float pass24 = SteadyStatePeak(f24, 50.0f);
    EXPECT_NEAR(pass12, 1.0f, 0.02f);
    EXPECT_NEAR(pass24, 1.0f, 0.02f);

    const float db12 = 20.0f * std::log10(SteadyStatePeak(f12, 4000.0f));
    const float db24 = 20.0f * std::log10(SteadyStatePeak(f24, 4000.0f));
    EXPECT_LT(db12, -20.0f);        // two octaves up, one stage
    EXPECT_LT(db24, db12 - 15.0f);  // the second stage adds its own roll-off
    EXPECT_NEAR(db24, 2.0f * db12, 6.0f);
}

TEST(SvfFilterTest, SlopeDefaultsToTwelveAndBypassStillHolds) {
    SvfFilter f = MakeFilter(1.0e6f);
    EXPECT_EQ(f.GetSlope(), SvfFilter::Slope::Db12);
    f.SetSlope(SvfFilter::Slope::Db24);
    f.SetDrive(1.0f);
    EXPECT_FLOAT_EQ(f.Process(0.7f), 0.7f) << "bypass must ignore slope and drive";
}

// --- Drive -------------------------------------------------------------------
//
// Drive 0 is the linear filter exactly; drive > 0 raises and soft-saturates
// the input (1x .. 2.5x, the ladders' law), blended in continuously. It
// never touches the integrator loop: the resonance is what the resonance
// setting asks for at every drive.

TEST(SvfFilterTest, ZeroDriveIsBitIdenticalToLinear) {
    SvfFilter linear = MakeFilter(1000.0f, 0.8f);
    SvfFilter zero = MakeFilter(1000.0f, 0.8f);
    zero.SetDrive(0.0f);
    for (int i = 0; i < 4000; ++i) {
        const float in = std::sin(2.0f * kPi * 990.0f * static_cast<float>(i) / 48000.0f);
        ASSERT_FLOAT_EQ(linear.Process(in), zero.Process(in)) << "sample " << i;
    }
}

TEST(SvfFilterTest, DriveLeavesTheResonantPeakAloneAndRaisesQuietSignalsByItsGain) {
    // Full resonance, a full-scale sine sitting on the cutoff: the linear
    // filter's peak is Q-sized, and drive does not damp it - the saturated
    // input is at most unity, so the driven peak is at most the linear one
    // and stays finite.
    SvfFilter linear = MakeFilter(1000.0f, 1.0f);
    SvfFilter driven = MakeFilter(1000.0f, 1.0f);
    driven.SetDrive(1.0f);
    const float hot_linear = SteadyStatePeak(linear, 1000.0f);
    const float hot_driven = SteadyStatePeak(driven, 1000.0f);
    EXPECT_GT(hot_linear, 4.0f) << "sanity: the linear peak at Q=20 is large";
    EXPECT_GT(hot_driven, 0.5f * hot_linear) << "drive must not damp the resonance";
    // A full-scale sine saturated at 2.5x is nearly square: its fundamental
    // is up to 4/pi of the sine's, and that is all the peak can grow by.
    EXPECT_LE(hot_driven, hot_linear * 1.3f);
    EXPECT_TRUE(std::isfinite(hot_driven));

    // At -60 dB both cubics are indistinguishable from straight lines, so the
    // driven filter is the linear one raised by exactly the input gain (2.5x
    // at full drive, the same law as the ladders).
    SvfFilter quiet_linear = MakeFilter(1000.0f, 0.3f);
    SvfFilter quiet_driven = MakeFilter(1000.0f, 0.3f);
    quiet_driven.SetDrive(1.0f);
    float peak_l = 0.0f, peak_d = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const float in = 0.001f * std::sin(2.0f * kPi * 500.0f * static_cast<float>(i) / 48000.0f);
        const float l = quiet_linear.Process(in);
        const float d = quiet_driven.Process(in);
        if (i > 24000) {
            peak_l = std::max(peak_l, std::fabs(l));
            peak_d = std::max(peak_d, std::fabs(d));
        }
    }
    EXPECT_NEAR(peak_d / peak_l, 2.5f, 0.03f);
}

TEST(SvfFilterTest, DriveIsContinuousFromZeroAndLouderAsItRises) {
    // No step at the first non-zero setting, and level rises monotonically
    // with drive on a moderate resonant signal.
    float last = 0.0f, first = 0.0f;
    for (float drive: {0.0f, 0.01f, 0.1f, 0.3f, 0.6f, 1.0f}) {
        SvfFilter f = MakeFilter(1000.0f, 0.5f);
        f.SetDrive(drive);
        float peak = 0.0f;
        for (int i = 0; i < 24000; ++i) {
            const float in =
                0.5f * std::sin(2.0f * kPi * 700.0f * static_cast<float>(i) / 48000.0f);
            const float out = f.Process(in);
            if (i > 12000)
                peak = std::max(peak, std::fabs(out));
        }
        if (drive == 0.0f)
            first = peak;
        else if (drive == 0.01f)
            EXPECT_NEAR(peak, last, 0.02f * last) << "1% drive must not jump";
        else
            EXPECT_GT(peak, last) << "drive " << drive;
        last = peak;
    }
    EXPECT_GT(last, 1.5f * first) << "full drive is clearly louder than none";
}

TEST(SvfFilterTest, DriveIsClampedAndReadsBack) {
    SvfFilter f = MakeFilter(1000.0f);
    f.SetDrive(3.0f);
    EXPECT_FLOAT_EQ(f.GetDrive(), 1.0f);
    f.SetDrive(-1.0f);
    EXPECT_FLOAT_EQ(f.GetDrive(), 0.0f);
}

TEST(SvfFilterTest, DrivenTwentyFourDbSweepStaysFiniteAndBounded) {
    SvfFilter f = MakeFilter(1000.0f, 1.0f);
    f.SetSlope(SvfFilter::Slope::Db24);
    f.SetDrive(1.0f);
    float peak = 0.0f;
    for (int i = 0; i < 96000; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        f.SetCutoff(200.0f + 8000.0f * (0.5f - 0.5f * std::cos(2.0f * kPi * 0.5f * t)));
        const float out = f.Process(std::sin(2.0f * kPi * 440.0f * t));
        ASSERT_TRUE(std::isfinite(out)) << "sample " << i;
        peak = std::max(peak, std::fabs(out));
    }
    // Two resonant stages at Q=20 can reach Q^2 = 400x on a sine held at the
    // cutoff; the sweep never dwells there, and drive adds no gain of its
    // own beyond the 2.5x input stage. Finite and within that bound.
    EXPECT_LT(peak, 500.0f);
}

TEST(SvfFilterTest, CombinedTuningMatchesSequentialSettersWithoutResettingState) {
    for (uint32_t rate: {44100u, 48000u, 96000u}) {
        for (auto slope: {SvfFilter::Slope::Db12, SvfFilter::Slope::Db24}) {
            SvfFilter sequential, combined;
            sequential.Init(rate);
            combined.Init(rate);
            sequential.SetSlope(slope);
            combined.SetSlope(slope);
            sequential.SetDrive(0.75f);
            combined.SetDrive(0.75f);
            // Keep integrators live through every tuning/bypass transition.
            for (float cutoff: {1000.0f, 0.0f, -100.0f, 20000.0f, 1000000.0f, 440.0f})
                for (float resonance: {0.0f, 0.5f, 1.0f, 2.0f, -1.0f}) {
                    sequential.SetCutoff(cutoff);
                    sequential.SetResonance(resonance);
                    combined.SetParameters(cutoff, resonance);
                    for (int sample = 0; sample < 96; ++sample) {
                        const float input = sample % 17 == 0 ? 0.2f : -0.05f;
                        EXPECT_FLOAT_EQ(combined.Process(input), sequential.Process(input));
                    }
                }
        }
    }
}
