// Tests for the modulation matrix (roadmap Phase 2.5 item 4; design:
// docs/features/param-locks-and-modulation.md §3).
//
// The matrix is pure, so everything about it is checkable here: the curves,
// the depth scaling, that slots on one destination sum rather than overwrite,
// and the identities that stop a patch with no modulation from changing the
// sound at all.

#include "audio/mod_matrix.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace WaveX::AudioEngine;

namespace {

ModSlot Slot(
    uint8_t source, uint8_t dest, int16_t depth, uint8_t curve = CURVE_LINEAR, uint8_t flags = 0) {
    ModSlot s;
    s.source = source;
    s.dest = dest;
    s.depth = depth;
    s.curve = curve;
    s.flags = flags;
    return s;
}

constexpr int16_t kFullDepth = 32767;

// The most important property in the file: a voice with no modulation must be
// bit-identical to one with no matrix at all. Every multiplier is unity and
// the pan offset is zero, so composing them onto a voice is a no-op.
TEST(ModMatrix, EmptyMatrixIsAnIdentity) {
    ModSources sources;
    const ModDestinations out = EvaluateModMatrix(nullptr, 0, sources);
    EXPECT_FLOAT_EQ(out.cutoff_mul, 1.0f);
    EXPECT_FLOAT_EQ(out.gain_mul, 1.0f);
    EXPECT_FLOAT_EQ(out.pitch_mul, 1.0f);
    EXPECT_FLOAT_EQ(out.pan_offset, 0.0f);
}

TEST(ModMatrix, SlotWithZeroDepthIsAlsoAnIdentity) {
    ModSlot slots[] = {Slot(SRC_LFO1, DEST_CUTOFF, 0)};
    ModSources sources;
    sources.lfo1 = 1.0f;
    const ModDestinations out = EvaluateModMatrix(slots, 1, sources);
    EXPECT_FLOAT_EQ(out.cutoff_mul, 1.0f);
}

TEST(ModMatrix, UnroutedSourceOrDestIsIgnored) {
    ModSlot slots[] = {
        Slot(SRC_NONE, DEST_CUTOFF, kFullDepth),
        Slot(SRC_LFO1, DEST_NONE, kFullDepth),
    };
    ModSources sources;
    sources.lfo1 = 1.0f;
    const ModDestinations out = EvaluateModMatrix(slots, 2, sources);
    EXPECT_FLOAT_EQ(out.cutoff_mul, 1.0f);
}

TEST(ModMatrix, FullDepthCutoffMovesTheDocumentedOctaves) {
    ModSlot slots[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth)};
    ModSources sources;

    sources.lfo1 = 1.0f;
    EXPECT_NEAR(
        EvaluateModMatrix(slots, 1, sources).cutoff_mul, std::pow(2.0f, kModCutoffOctaves), 0.01f);

    sources.lfo1 = -1.0f;
    EXPECT_NEAR(EvaluateModMatrix(slots, 1, sources).cutoff_mul,
                std::pow(2.0f, -kModCutoffOctaves),
                0.001f);
}

// Frequency modulation has to be multiplicative, or the same depth would mean
// something different at 200 Hz than at 2 kHz.
TEST(ModMatrix, CutoffIsMultiplicativeAndSymmetricInOctaves) {
    ModSlot slots[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth)};
    ModSources up, down;
    up.lfo1 = 0.5f;
    down.lfo1 = -0.5f;

    const float a = EvaluateModMatrix(slots, 1, up).cutoff_mul;
    const float b = EvaluateModMatrix(slots, 1, down).cutoff_mul;
    EXPECT_NEAR(a * b, 1.0f, 0.001f) << "equal and opposite depths must cancel";
}

TEST(ModMatrix, PitchFullDepthIsTheDocumentedSemitones) {
    ModSlot slots[] = {Slot(SRC_LFO_VOICE, DEST_PITCH, kFullDepth)};
    ModSources sources;
    sources.lfo_voice = 1.0f;
    const float expected = std::pow(2.0f, kModPitchSemitones / 12.0f);
    EXPECT_NEAR(EvaluateModMatrix(slots, 1, sources).pitch_mul, expected, 0.0001f);
}

// A full negative gain depth must mute, not invert. An inverted waveform is
// not "quieter" - it is the same level with the phase flipped.
TEST(ModMatrix, GainFloorsAtSilenceRatherThanInverting) {
    ModSlot slots[] = {Slot(SRC_MACRO_1, DEST_GAIN, -kFullDepth)};
    ModSources sources;
    sources.macro[0] = 1.0f;
    const ModDestinations out = EvaluateModMatrix(slots, 1, sources);
    EXPECT_FLOAT_EQ(out.gain_mul, 0.0f);

    // And well past full depth still floors rather than going negative.
    ModSlot deep[] = {Slot(SRC_MACRO_1, DEST_GAIN, -kFullDepth),
                      Slot(SRC_MACRO_2, DEST_GAIN, -kFullDepth)};
    sources.macro[1] = 1.0f;
    EXPECT_FLOAT_EQ(EvaluateModMatrix(deep, 2, sources).gain_mul, 0.0f);
}

TEST(ModMatrix, PanOffsetIsClampedToTheVoiceRange) {
    ModSlot slots[] = {Slot(SRC_MACRO_1, DEST_PAN, kFullDepth),
                       Slot(SRC_MACRO_2, DEST_PAN, kFullDepth)};
    ModSources sources;
    sources.macro[0] = 1.0f;
    sources.macro[1] = 1.0f;
    EXPECT_FLOAT_EQ(EvaluateModMatrix(slots, 2, sources).pan_offset, 1.0f);
}

// Two sources on one destination must behave like a mixer, not last-one-wins.
TEST(ModMatrix, SlotsOnTheSameDestinationSum) {
    ModSlot one[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth / 2)};
    ModSlot two[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth / 2),
                     Slot(SRC_LFO2, DEST_CUTOFF, kFullDepth / 2)};
    ModSources sources;
    sources.lfo1 = 1.0f;
    sources.lfo2 = 1.0f;

    const float single = EvaluateModMatrix(one, 1, sources).cutoff_mul;
    const float summed = EvaluateModMatrix(two, 2, sources).cutoff_mul;
    EXPECT_GT(summed, single) << "the second slot was ignored";
    // Two half-depth slots at full source == one full-depth slot.
    ModSlot full[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth)};
    EXPECT_NEAR(summed, EvaluateModMatrix(full, 1, sources).cutoff_mul, 0.05f);
}

TEST(ModMatrix, OpposingSlotsCancel) {
    ModSlot slots[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth),
                       Slot(SRC_LFO2, DEST_CUTOFF, -kFullDepth)};
    ModSources sources;
    sources.lfo1 = 1.0f;
    sources.lfo2 = 1.0f;
    EXPECT_NEAR(EvaluateModMatrix(slots, 2, sources).cutoff_mul, 1.0f, 0.001f);
}

TEST(ModMatrix, DestinationsAreIndependent) {
    ModSlot slots[] = {Slot(SRC_LFO1, DEST_CUTOFF, kFullDepth)};
    ModSources sources;
    sources.lfo1 = 1.0f;
    const ModDestinations out = EvaluateModMatrix(slots, 1, sources);
    EXPECT_GT(out.cutoff_mul, 1.0f);
    EXPECT_FLOAT_EQ(out.gain_mul, 1.0f) << "cutoff modulation leaked into gain";
    EXPECT_FLOAT_EQ(out.pitch_mul, 1.0f);
    EXPECT_FLOAT_EQ(out.pan_offset, 0.0f);
}

TEST(ModMatrix, UnipolarRemapCentresAUnipolarSource) {
    // A macro at 0.5 remapped to bipolar is 0, i.e. no modulation.
    ModSlot slots[] = {
        Slot(SRC_MACRO_1, DEST_CUTOFF, kFullDepth, CURVE_LINEAR, MOD_FLAG_UNIPOLAR_TO_BIPOLAR)};
    ModSources sources;
    sources.macro[0] = 0.5f;
    EXPECT_NEAR(EvaluateModMatrix(slots, 1, sources).cutoff_mul, 1.0f, 0.001f);

    // At 0 it should modulate fully downward.
    sources.macro[0] = 0.0f;
    EXPECT_LT(EvaluateModMatrix(slots, 1, sources).cutoff_mul, 1.0f);
}

TEST(ModCurves, AllAreExactAtTheEndpoints) {
    for (uint8_t curve: {CURVE_LINEAR, CURVE_EXPONENTIAL, CURVE_S}) {
        EXPECT_FLOAT_EQ(ApplyModCurve(0.0f, curve), 0.0f) << "curve " << int(curve);
        EXPECT_FLOAT_EQ(ApplyModCurve(1.0f, curve), 1.0f) << "curve " << int(curve);
        EXPECT_FLOAT_EQ(ApplyModCurve(-1.0f, curve), -1.0f) << "curve " << int(curve);
    }
}

// Sign preservation is what stops a bipolar source folding: without it a
// negative LFO half would come back positive and the vibrato would double in
// frequency while losing its downward swing.
TEST(ModCurves, PreserveSign) {
    for (uint8_t curve: {CURVE_LINEAR, CURVE_EXPONENTIAL, CURVE_S}) {
        EXPECT_LT(ApplyModCurve(-0.5f, curve), 0.0f) << "curve " << int(curve) << " folded";
        EXPECT_GT(ApplyModCurve(0.5f, curve), 0.0f);
        EXPECT_FLOAT_EQ(ApplyModCurve(-0.5f, curve), -ApplyModCurve(0.5f, curve));
    }
}

TEST(ModCurves, ExponentialGivesFinerControlNearZero) {
    EXPECT_LT(ApplyModCurve(0.25f, CURVE_EXPONENTIAL), ApplyModCurve(0.25f, CURVE_LINEAR));
    EXPECT_LT(ApplyModCurve(0.75f, CURVE_EXPONENTIAL), ApplyModCurve(0.75f, CURVE_LINEAR));
}

TEST(ModCurves, SCurveIsSlowAtBothEndsAndMidwayAtTheMiddle) {
    EXPECT_LT(ApplyModCurve(0.25f, CURVE_S), ApplyModCurve(0.25f, CURVE_LINEAR));
    EXPECT_GT(ApplyModCurve(0.75f, CURVE_S), ApplyModCurve(0.75f, CURVE_LINEAR));
    EXPECT_NEAR(ApplyModCurve(0.5f, CURVE_S), 0.5f, 0.0001f);
}

TEST(ModMatrix, AllEightSlotsAreEvaluated) {
    ModSlot slots[kMaxModSlots];
    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        slots[i] = Slot(SRC_MACRO_1, DEST_CUTOFF, kFullDepth / 8);
    }
    ModSources sources;
    sources.macro[0] = 1.0f;

    ModSlot one[] = {Slot(SRC_MACRO_1, DEST_CUTOFF, kFullDepth)};
    EXPECT_NEAR(EvaluateModMatrix(slots, kMaxModSlots, sources).cutoff_mul,
                EvaluateModMatrix(one, 1, sources).cutoff_mul,
                0.2f);
}

// The analog stage is deferred, so this source must contribute nothing rather
// than reading uninitialised state.
TEST(ModMatrix, ParaEnvSourceIsInertWhileAnalogIsDeferred) {
    ModSlot slots[] = {Slot(SRC_PARA_ENV, DEST_CUTOFF, kFullDepth)};
    ModSources sources;
    EXPECT_FLOAT_EQ(EvaluateModMatrix(slots, 1, sources).cutoff_mul, 1.0f);
}

TEST(ModMatrix, CountIsClampedToTheSlotLimit) {
    ModSlot slots[kMaxModSlots];
    for (uint8_t i = 0; i < kMaxModSlots; ++i) {
        slots[i] = Slot(SRC_MACRO_1, DEST_GAIN, 0);
    }
    ModSources sources;
    // An over-large count must not read past the array.
    EXPECT_FLOAT_EQ(EvaluateModMatrix(slots, 200, sources).gain_mul, 1.0f);
}

}  // namespace
