#pragma once

// Modulation matrix (roadmap Phase 2.5 item 4; design:
// docs/features/param-locks-and-modulation.md §3).
//
// Eight instrument-scoped slots, each routing one source to one destination
// with a depth and a curve. Evaluated once per control tick per active voice
// and applied at block rate - never per sample. The design's cost ceiling is 8
// slots x 8 voices x ~10 ops, which is why this can stay a straightforward
// loop rather than anything clever.
//
// Evaluate() reads a ModSources snapshot and writes ModDestinations.
// An optional voice-owned cache reuses unchanged exponential mappings.
// Every curve, accumulation and full-scale range remains host-testable.
// The caller decides when
// sources are sampled - per-trigger sources (velocity, note, random) are
// captured into the voice at note-on and are constants thereafter, exactly as
// §3 requires.

#include "memory_sections.h"
#include "spi_protocol/protocol.h"

#include <cmath>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

/// Slots per instrument (§3).
static constexpr uint8_t kMaxModSlots = 8;

/// Wire-stable ordering; do not renumber. SRC_PARA_ENV is retained so the
/// numbering does not shift, but the analog stage it fed is deferred (roadmap
/// § Analog CV is deferred) and it always evaluates to 0.
enum ModSource : uint8_t {
    SRC_NONE = 0,
    SRC_VELOCITY,    // per-trigger, 0..1
    SRC_NOTE,        // per-trigger, 0..1 across the MIDI range
    SRC_ENV_FILTER,  // per-voice second envelope, 0..1
    SRC_LFO1,        // global, -1..+1
    SRC_LFO2,        // retired global source; always zero
    SRC_LFO_VOICE,   // per-voice, -1..+1
    SRC_RANDOM,      // per-trigger sample & hold, -1..+1
    SRC_MACRO_1,     // 0..1
    SRC_MACRO_2,
    SRC_MACRO_3,
    SRC_MACRO_4,
    SRC_MODWHEEL,    // MIDI CC1, 0..1
    SRC_AFTERTOUCH,  // channel pressure, 0..1
    SRC_PARA_ENV,    // deferred with the analog stage; always 0
    SRC_ENV_AMP,     // Env 1, sampled from the audio-rate amp envelope
    SRC_ENV_AUX,     // Env 3, block-rate modulation envelope
    SRC_LFO_VOICE2,  // second per-voice LFO
    SRC_COUNT
};

enum ModDest : uint8_t {
    DEST_NONE = 0,
    DEST_CUTOFF,
    DEST_GAIN,
    DEST_PITCH,
    DEST_PAN,
    DEST_RESONANCE = Protocol::INST_MOD_RESONANCE,
    DEST_OSC1_PITCH = Protocol::INST_MOD_OSC1_PITCH,
    DEST_OSC2_PITCH = Protocol::INST_MOD_OSC2_PITCH,
    DEST_COUNT
};

static_assert(SRC_COUNT == Protocol::INST_MOD_SOURCE_COUNT, "modulation source wire contract");
static_assert(DEST_COUNT == Protocol::INST_MOD_DEST_COUNT, "modulation destination wire contract");
enum ModCurve : uint8_t {
    CURVE_LINEAR = 0,
    CURVE_EXPONENTIAL,  // more travel near zero
    CURVE_S,            // slow at both ends
};

enum ModSlotFlags : uint8_t {
    MOD_FLAG_UNIPOLAR_TO_BIPOLAR = 1 << 0,  // remap a 0..1 source onto -1..+1
};

struct ModSlot {
    uint8_t source = SRC_NONE;
    uint8_t dest = DEST_NONE;
    int16_t depth = 0;  // +-32767 maps to +-100%
    uint8_t curve = CURVE_LINEAR;
    uint8_t flags = 0;
};

/// A snapshot of every source at one control tick. Per-trigger sources are
/// already resolved to constants by the caller.
struct ModSources {
    float velocity = 0.0f;
    float note = 0.0f;
    float env_filter = 0.0f;
    float env_amp = 0.0f, env_aux = 0.0f;
    float lfo1 = 0.0f;
    float lfo_voice = 0.0f;
    float lfo_voice2 = 0.0f;
    float random = 0.0f;
    float macro[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float modwheel = 0.0f;
    float aftertouch = 0.0f;

    float Get(uint8_t source) const {
        switch (source) {
            case SRC_VELOCITY:
                return velocity;
            case SRC_NOTE:
                return note;
            case SRC_ENV_FILTER:
                return env_filter;
            case SRC_ENV_AMP:
                return env_amp;
            case SRC_ENV_AUX:
                return env_aux;
            case SRC_LFO1:
                return lfo1;
            case SRC_LFO2:
                return 0;
            case SRC_LFO_VOICE:
                return lfo_voice;
            case SRC_LFO_VOICE2:
                return lfo_voice2;
            case SRC_RANDOM:
                return random;
            case SRC_MACRO_1:
                return macro[0];
            case SRC_MACRO_2:
                return macro[1];
            case SRC_MACRO_3:
                return macro[2];
            case SRC_MACRO_4:
                return macro[3];
            case SRC_MODWHEEL:
                return modwheel;
            case SRC_AFTERTOUCH:
                return aftertouch;
            case SRC_PARA_ENV:  // deferred with the analog stage
            case SRC_NONE:
            default:
                return 0.0f;
        }
    }
};

// Voice-owned memoization of the four exponential destination mappings.
// Keys are the final base-2 exponents, so changed routes/curves need no
// separate invalidation and identical sums produce identical sound.
struct ModScaleCache {
    float exponent[4] = {};
    float value[4] = {1, 1, 1, 1};
    float Scale(uint8_t index, float power) {
        if (power != exponent[index]) {
            exponent[index] = power;
            value[index] = power != 0.f ? std::pow(2.f, power) : 1.f;
        }
        return value[index];
    }
};

/**
 * @brief What the matrix produces, applied at the top of a voice's block.
 *
 * Multipliers rather than absolute values, so modulation composes onto
 * whatever the zone and the live params already set instead of replacing it.
 * `pan_offset` is additive to match the mixer's convention (track_mix.hpp), so
 * there is one meaning of "pan offset" in the engine rather than two.
 */
struct ModDestinations {
    float cutoff_mul = 1.0f;
    float gain_mul = 1.0f;
    float pitch_mul = 1.0f;
    float oscillator_pitch_mul[2] = {1.0f, 1.0f};
    float pan_offset = 0.0f;
    float resonance_offset = 0.0f;
};

// Full-scale ranges. These are CHOSEN, not derived - the design fixes the
// evaluation model but not how much a depth of 100% should move each
// destination. Named so they can be tuned by ear later without hunting through
// arithmetic, and picked to be musically useful rather than maximal:
//
//   - Four octaves of cutoff is a filter sweep across the audible range.
//   - Two semitones of pitch is vibrato; wider is a pitch envelope, which is
//     what a deeper depth on a slower source gives you anyway.
//   - Gain is scaled linearly and floored at silence, so a full negative depth
//     mutes rather than inverting the waveform.
static constexpr float kModCutoffOctaves = 4.0f;
static constexpr float kModPitchSemitones = 2.0f;

/// Applies a slot's curve. Sign-preserving, so a bipolar source keeps its
/// shape either side of zero rather than folding.
inline float ApplyModCurve(float value, uint8_t curve) {
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    const float magnitude = value < 0.0f ? -value : value;
    switch (curve) {
        case CURVE_EXPONENTIAL:
            // Quadratic rather than a true exponential: it has the same
            // "fine control near zero" feel, costs one multiply, and unlike
            // exp() it is exactly 0 at 0 and exactly 1 at 1.
            return sign * magnitude * magnitude;
        case CURVE_S:
            // Smoothstep, computed rather than looked up. The design says a
            // 33-point LUT; this is cheaper, needs no table, and is exact at
            // both ends and the midpoint.
            return sign * magnitude * magnitude * (3.0f - 2.0f * magnitude);
        case CURVE_LINEAR:
        default:
            return value;
    }
}

/**
 * @brief Accumulates every slot into a set of block-rate destinations.
 *
 * @param slots     the instrument's slots
 * @param count     how many are populated (<= kMaxModSlots)
 * @param sources   this tick's source snapshot
 * @param cache     optional voice-owned derived exponential mappings
 *
 * Slots targeting the same destination sum, which is what makes two sources
 * on one cutoff behave like a mixer rather than last-one-wins.
 */
WAVEX_ITCM_CODE_NAMED("voice.EvaluateModMatrix")
inline ModDestinations EvaluateModMatrix(const ModSlot* slots,
                                         uint8_t count,
                                         const ModSources& sources,
                                         ModScaleCache* cache = nullptr) {
    float cutoff = 0.0f;
    float gain = 0.0f;
    float pitch = 0.0f;
    float oscillator_pitch[2] = {};
    float pan = 0.0f;
    float resonance = 0.0f;

    if (slots) {
        if (count > kMaxModSlots) {
            count = kMaxModSlots;
        }
        for (uint8_t i = 0; i < count; ++i) {
            const ModSlot& slot = slots[i];
            if (slot.source == SRC_NONE || slot.dest == DEST_NONE || slot.depth == 0) {
                continue;
            }
            float value = sources.Get(slot.source);
            if (slot.flags & MOD_FLAG_UNIPOLAR_TO_BIPOLAR) {
                value = value * 2.0f - 1.0f;
            }
            const float depth = static_cast<float>(slot.depth) / 32767.0f;
            const float amount = depth * ApplyModCurve(value, slot.curve);

            switch (slot.dest) {
                case DEST_CUTOFF:
                    cutoff += amount;
                    break;
                case DEST_GAIN:
                    gain += amount;
                    break;
                case DEST_PITCH:
                    pitch += amount;
                    break;
                case DEST_OSC1_PITCH:
                    oscillator_pitch[0] += amount;
                    break;
                case DEST_OSC2_PITCH:
                    oscillator_pitch[1] += amount;
                    break;
                case DEST_RESONANCE:
                    resonance += amount;
                    break;
                case DEST_PAN:
                    pan += amount;
                    break;
                default:
                    break;
            }
        }
    }

    ModDestinations out;
    const auto scale = [cache](uint8_t index, float power) {
        return cache ? cache->Scale(index, power) : (power != 0.f ? std::pow(2.f, power) : 1.f);
    };
    // Exponential in octaves: modulation of a frequency has to be
    // multiplicative or the same depth means something different at 200 Hz
    // than at 2 kHz.
    out.cutoff_mul = scale(0, cutoff * kModCutoffOctaves);
    out.gain_mul = 1.0f + gain;
    if (out.gain_mul < 0.0f) {
        out.gain_mul = 0.0f;  // silence, never a phase-inverted signal
    }
    out.pitch_mul = scale(1, (pitch * kModPitchSemitones) / 12.0f);
    // Reuse the common pitch range; inactive or cancelling routes retain
    // identity without evaluating another exponential.
    for (uint8_t i = 0; i < 2; ++i) {
        out.oscillator_pitch_mul[i] =
            scale(static_cast<uint8_t>(2 + i), (oscillator_pitch[i] * kModPitchSemitones) / 12.0f);
    }
    out.pan_offset = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
    // Full signed depth spans the normalized resonance range. Sum routes
    // before clamping; the voice adds this to its own (possibly locked) base.
    out.resonance_offset = resonance < -1.f ? -1.f : (resonance > 1.f ? 1.f : resonance);
    return out;
}

}  // namespace AudioEngine
}  // namespace WaveX
