// Tests for the per-track mixer model (roadmap Phase 2.5 item 2).
//
// Covers the four properties docs/features/output-routing-and-mixer.md §6 asks
// for: mute-ramp continuity, solo-set to mute-set expansion, dB<->linear
// goldens, and meter byte monotonicity. Everything here is pure arithmetic
// shared by both MCUs, so a disagreement would show up as a fader that does
// not match what is heard - which is exactly the class of bug a single tested
// implementation exists to prevent.

#include "audio/track_mix.hpp"

#include <gtest/gtest.h>

#include <cmath>

using WaveX::Mix::kMinGainDb;
using WaveX::Mix::kMuteRampSeconds;
using WaveX::Mix::kNumTracks;
using WaveX::Mix::TrackMixer;

namespace {

constexpr float kSampleRate = 48000.0f;
constexpr uint32_t kBlock = 48;  // 1 ms, so a 5 ms ramp spans five blocks

TEST(TrackMixDb, RoundTripsThroughLinear) {
    for (float db = -59.0f; db <= 6.0f; db += 1.0f) {
        const float linear = WaveX::Mix::DbToLinear(db);
        EXPECT_NEAR(WaveX::Mix::LinearToDb(linear), db, 0.001f) << "at " << db << " dB";
    }
}

TEST(TrackMixDb, KnownPoints) {
    EXPECT_NEAR(WaveX::Mix::DbToLinear(0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(WaveX::Mix::DbToLinear(-6.0f), 0.5012f, 1e-3f);
    EXPECT_NEAR(WaveX::Mix::DbToLinear(6.0f), 1.9953f, 1e-3f);
}

// A fader at the bottom must be silent. Returning a very small but non-zero
// gain would leave a quiet passage audible under a closed fader.
TEST(TrackMixDb, FloorIsTrueSilence) {
    EXPECT_EQ(WaveX::Mix::DbToLinear(kMinGainDb), 0.0f);
    EXPECT_EQ(WaveX::Mix::DbToLinear(-120.0f), 0.0f);
}

// Formatting a label from this must never produce nan or -inf.
TEST(TrackMixDb, NonPositiveAmplitudeReportsTheFloor) {
    EXPECT_EQ(WaveX::Mix::LinearToDb(0.0f), kMinGainDb);
    EXPECT_EQ(WaveX::Mix::LinearToDb(-1.0f), kMinGainDb);
}

TEST(TrackMixSolo, NoSoloLeavesExplicitMutesAlone) {
    EXPECT_EQ(WaveX::Mix::ExpandSoloToMutes(0x0000, 0x0000), 0x0000);
    EXPECT_EQ(WaveX::Mix::ExpandSoloToMutes(0x0000, 0x0005), 0x0005);
}

TEST(TrackMixSolo, SoloMutesEverythingElse) {
    // Track 3 soloed, nothing explicitly muted: every other track mutes.
    const uint16_t mutes = WaveX::Mix::ExpandSoloToMutes(1u << 3, 0x0000);
    EXPECT_EQ((mutes >> 3) & 1u, 0u) << "the soloed track was muted";
    EXPECT_EQ((mutes >> 0) & 1u, 1u);
    EXPECT_EQ((mutes >> 15) & 1u, 1u);
}

// Soloing a track you had muted must let you hear it - that is most of what
// solo is for when checking a mix.
TEST(TrackMixSolo, SoloOverridesAnExplicitMuteOnTheSoloedTrack) {
    const uint16_t mutes = WaveX::Mix::ExpandSoloToMutes(1u << 2, 0xFFFF);
    EXPECT_EQ((mutes >> 2) & 1u, 0u);
}

TEST(TrackMixSolo, MultipleSolosPlayTogether) {
    const uint16_t mutes = WaveX::Mix::ExpandSoloToMutes((1u << 1) | (1u << 4), 0x0000);
    EXPECT_EQ((mutes >> 1) & 1u, 0u);
    EXPECT_EQ((mutes >> 4) & 1u, 0u);
    EXPECT_EQ((mutes >> 2) & 1u, 1u);
}

TEST(TrackMixMeter, SilenceIsDistinctFromVeryQuiet) {
    EXPECT_EQ(WaveX::Mix::PeakToMeterByte(0.0f), 0);
    // -40 dB: quiet, but well inside the meter's range.
    EXPECT_GT(WaveX::Mix::PeakToMeterByte(0.01f), 0) << "a real signal read as silence";
}

// The floor is a real boundary, not an accident: a peak at or below kMinGainDb
// reads as 0, the same as true silence. Pinned because it is the one place the
// meter deliberately loses information, and because 0.001 is exactly -60 dB -
// which is how the test above originally managed to fail against correct code.
TEST(TrackMixMeter, AtOrBelowTheFloorReadsAsSilence) {
    EXPECT_EQ(WaveX::Mix::PeakToMeterByte(WaveX::Mix::DbToLinear(kMinGainDb)), 0);
    EXPECT_EQ(WaveX::Mix::PeakToMeterByte(0.001f), 0) << "0.001 is exactly -60 dB";
    EXPECT_GT(WaveX::Mix::PeakToMeterByte(0.0011f), 0);
}

TEST(TrackMixMeter, IsMonotonicInPeak) {
    uint8_t previous = 0;
    for (int i = 0; i <= 1000; ++i) {
        const float peak = static_cast<float>(i) / 1000.0f;
        const uint8_t value = WaveX::Mix::PeakToMeterByte(peak);
        EXPECT_GE(value, previous) << "meter went backwards at peak " << peak;
        previous = value;
    }
    EXPECT_EQ(WaveX::Mix::PeakToMeterByte(1.0f), 255);
}

TEST(TrackMixMeter, ByteRoundTripsToApproximatelyTheSameDb) {
    for (float db = -55.0f; db <= 0.0f; db += 5.0f) {
        const uint8_t value = WaveX::Mix::PeakToMeterByte(WaveX::Mix::DbToLinear(db));
        // 60 dB across 254 steps is ~0.24 dB per step.
        EXPECT_NEAR(WaveX::Mix::MeterByteToDb(value), db, 0.5f) << "at " << db << " dB";
    }
}

TEST(TrackMixer, StartsOpenSoNothingFadesInOnBoot) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    for (uint8_t t = 0; t < kNumTracks; ++t) {
        EXPECT_EQ(mixer.GainFor(t), 1.0f) << "track " << int(t);
    }
    EXPECT_TRUE(mixer.Settled());
}

// The property that makes a mute click-free: the gain never jumps. A hard cut
// would show up here as a single step of 1.0.
TEST(TrackMixer, MuteRampsWithoutADiscontinuity) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetMute(0, true);

    // One block is 1 ms of a 5 ms ramp, so no step may exceed ~0.2 plus slack.
    const float max_step = (static_cast<float>(kBlock) / (kMuteRampSeconds * kSampleRate)) + 1e-4f;
    float previous = mixer.GainFor(0);
    int blocks = 0;
    while (!mixer.Settled() && blocks < 1000) {
        mixer.Tick(kBlock);
        const float now = mixer.GainFor(0);
        EXPECT_LE(std::fabs(now - previous), max_step) << "discontinuity at block " << blocks;
        previous = now;
        ++blocks;
    }
    EXPECT_TRUE(mixer.Settled());
    EXPECT_EQ(mixer.GainFor(0), 0.0f);
    // 5 ms at 1 ms per block.
    EXPECT_LE(blocks, 6);
    EXPECT_GE(blocks, 5);
}

TEST(TrackMixer, UnmuteRampsBackUpSymmetrically) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetMute(1, true);
    for (int i = 0; i < 10; ++i) {
        mixer.Tick(kBlock);
    }
    ASSERT_EQ(mixer.GainFor(1), 0.0f);

    mixer.SetMute(1, false);
    const float max_step = (static_cast<float>(kBlock) / (kMuteRampSeconds * kSampleRate)) + 1e-4f;
    float previous = mixer.GainFor(1);
    int blocks = 0;
    while (!mixer.Settled() && blocks < 1000) {
        mixer.Tick(kBlock);
        const float now = mixer.GainFor(1);
        EXPECT_LE(std::fabs(now - previous), max_step);
        previous = now;
        ++blocks;
    }
    EXPECT_EQ(mixer.GainFor(1), 1.0f);
}

// Reversing mid-ramp must continue from where it is, not restart from an
// endpoint - restarting is itself a discontinuity.
TEST(TrackMixer, ReversingMidRampIsContinuous) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetMute(2, true);
    mixer.Tick(kBlock);
    mixer.Tick(kBlock);
    const float mid = mixer.GainFor(2);
    ASSERT_GT(mid, 0.0f);
    ASSERT_LT(mid, 1.0f);

    mixer.SetMute(2, false);
    mixer.Tick(kBlock);
    const float after = mixer.GainFor(2);
    EXPECT_GT(after, mid) << "unmute did not resume from the current level";
    EXPECT_LT(after, 1.0f) << "unmute jumped straight to open";
}

// A block longer than the ramp must land exactly on the target rather than
// overshooting into a negative gain.
TEST(TrackMixer, ABlockLongerThanTheRampSettlesExactly) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetMute(3, true);
    mixer.Tick(48000);  // a full second
    EXPECT_EQ(mixer.GainFor(3), 0.0f);
    EXPECT_TRUE(mixer.Settled());
}

TEST(TrackMixer, GainComposesWithTheMuteRamp) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetGain(4, 0.5f);
    EXPECT_NEAR(mixer.GainFor(4), 0.5f, 1e-6f);

    mixer.SetMute(4, true);
    mixer.Tick(48000);
    EXPECT_EQ(mixer.GainFor(4), 0.0f) << "mute must win over a set gain";
}

TEST(TrackMixer, SetMuteMaskAppliesAWholeSoloExpansion) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetMuteMask(WaveX::Mix::ExpandSoloToMutes(1u << 5, 0x0000));
    mixer.Tick(48000);

    EXPECT_EQ(mixer.GainFor(5), 1.0f);
    for (uint8_t t = 0; t < kNumTracks; ++t) {
        if (t != 5) {
            EXPECT_EQ(mixer.GainFor(t), 0.0f) << "track " << int(t) << " was not muted";
        }
    }
}

TEST(TrackMixer, OutOfRangeTrackIsInertRatherThanCorrupting) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetGain(kNumTracks, 0.25f);
    mixer.SetMute(kNumTracks + 7, true);
    mixer.SetPanOffset(200, 1.0f);
    mixer.Tick(kBlock);

    EXPECT_EQ(mixer.GainFor(kNumTracks), 0.0f) << "an out-of-range read must not alias track 0";
    for (uint8_t t = 0; t < kNumTracks; ++t) {
        EXPECT_EQ(mixer.GainFor(t), 1.0f) << "track " << int(t) << " was disturbed";
    }
}

TEST(TrackMixer, PanOffsetClamps) {
    TrackMixer mixer;
    mixer.SetPanOffset(0, 5.0f);
    EXPECT_EQ(mixer.PanOffsetFor(0), 1.0f);
    mixer.SetPanOffset(0, -5.0f);
    EXPECT_EQ(mixer.PanOffsetFor(0), -1.0f);
}

TEST(TrackMixer, ResetDoesNotFadeEverythingBackIn) {
    TrackMixer mixer;
    mixer.SetSampleRate(kSampleRate);
    mixer.SetMute(0, true);
    mixer.Tick(48000);
    ASSERT_EQ(mixer.GainFor(0), 0.0f);

    mixer.Reset();
    EXPECT_EQ(mixer.GainFor(0), 1.0f);
    EXPECT_TRUE(mixer.Settled()) << "reset left a ramp running";
}

// --- wire encoding (MSG_MIX_OP) -------------------------------------------

TEST(TrackMixWire, GainAnchorPoints) {
    EXPECT_EQ(WaveX::Mix::GainDbToWire(kMinGainDb), 0) << "the floor must encode as 0";
    EXPECT_EQ(WaveX::Mix::GainDbToWire(0.0f), 6000);
    EXPECT_EQ(WaveX::Mix::GainDbToWire(6.0f), 6600);
}

TEST(TrackMixWire, GainRoundTripsWithinAStep) {
    for (float db = kMinGainDb; db <= 6.0f; db += 0.37f) {
        const uint16_t wire = WaveX::Mix::GainDbToWire(db);
        EXPECT_NEAR(WaveX::Mix::WireToGainDb(wire), db, 0.01f) << "at " << db << " dB";
    }
}

TEST(TrackMixWire, GainClampsRatherThanWrapping) {
    // Unsigned wire field: an out-of-range dB must saturate, never wrap to a
    // huge value that would decode as a loud track.
    EXPECT_EQ(WaveX::Mix::GainDbToWire(-200.0f), 0);
    EXPECT_EQ(WaveX::Mix::GainDbToWire(200.0f), WaveX::Mix::GainDbToWire(6.0f));
    EXPECT_LE(WaveX::Mix::WireToGainDb(65535), 6.0f);
}

TEST(TrackMixWire, PanMatchesTheParamPanConvention) {
    EXPECT_EQ(WaveX::Mix::PanToWire(-1.0f), 0);
    EXPECT_EQ(WaveX::Mix::PanToWire(1.0f), 65535);
    // Centre is 32768 per PARAM_PAN's documented convention.
    EXPECT_NEAR(static_cast<float>(WaveX::Mix::PanToWire(0.0f)), 32768.0f, 1.0f);
    EXPECT_NEAR(WaveX::Mix::WireToPan(32768), 0.0f, 0.001f);
}

TEST(TrackMixWire, PanRoundTrips) {
    for (float pan = -1.0f; pan <= 1.0f; pan += 0.1f) {
        EXPECT_NEAR(WaveX::Mix::WireToPan(WaveX::Mix::PanToWire(pan)), pan, 0.001f);
    }
}

}  // namespace
