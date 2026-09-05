#pragma once

// Sequencer pattern data model (roadmap Phase 2 item 2; design:
// docs/features/sequencer.md §3). HAL-free plain structs - no wire
// (__attribute__((packed))) requirements here, these are engine-internal,
// not sent over the inter-MCU link verbatim (protocol messages that edit
// them are a separate, later stage - see docs/features/sequencer.md §4).
//
// A "kit" (docs/features/instrument-model.md §8) is exactly a drum-mode
// Instrument; nothing sequencer-specific hardcodes drum semantics here -
// Track/Step apply equally to melodic tracks once
// docs/features/melodic-sequencing.md's StepNotes lands (deliberately not
// included in this pass: that doc's parallel per-track note-lane array is
// a separate, additive extension, not a change to this struct).

#include <cstdint>

namespace WaveX {
namespace Sequencer {

static constexpr uint8_t kMaxSteps = 64;
static constexpr uint8_t kMaxTracks = 16;
static constexpr uint8_t kMaxParamLocks = 4;
static constexpr uint8_t kMaxRetrigCount = 8;

// Internal scheduling resolution (sequencer.md §2: "96 PPQN internally for
// micro-timing/swing resolution"). All tick-valued fields below (
// micro_offset, retrig_rate_ticks) are in these units.
static constexpr uint16_t kInternalPpqn = 96;

// Step interval, in internal PPQN ticks, per pattern-level note-length
// setting. Triplet values are exactly 2/3 of their straight counterpart
// (a triplet subdivision fits 3 notes in the space of 2 straight ones);
// kInternalPpqn=96 was chosen so every value below is an exact integer -
// do not add a scale whose triplet ticks would be fractional.
enum class StepScale : uint8_t {
    ThirtySecond = 0,  // 12 ticks
    Sixteenth,         // 24 ticks
    Eighth,            // 48 ticks
    Quarter,           // 96 ticks
    SixteenthTriplet,  // 16 ticks
    EighthTriplet,     // 32 ticks
};

inline uint16_t StepIntervalTicks(StepScale scale) {
    switch (scale) {
        case StepScale::ThirtySecond:
            return 12;
        case StepScale::Sixteenth:
            return 24;
        case StepScale::Eighth:
            return 48;
        case StepScale::Quarter:
            return 96;
        case StepScale::SixteenthTriplet:
            return 16;
        case StepScale::EighthTriplet:
            return 32;
    }
    return 24;  // unreachable for a valid enum value; Sixteenth is the sane default
}

// Per-step parameter override ("p-lock", Elektron-style). param_id is a
// ControlParameter (protocol.h) value, but this header deliberately does
// not depend on protocol.h - the scheduler core is transport-agnostic;
// application of locks to live parameters is
// docs/features/param-locks-and-modulation.md §2's job, one layer up.
// param_id == 0 marks an unused slot.
struct ParamLock {
    uint8_t param_id = 0;
    uint16_t value = 0;
};

struct Step {
    bool on = false;
    uint8_t velocity = 100;     // 0-127
    uint8_t probability = 100;  // 0-100: percent chance this step fires when reached
    // Signed offset from the pattern-level swing-adjusted step boundary, in
    // internal PPQN ticks. Applied on top of (not instead of) swing.
    int16_t micro_offset = 0;
    // 0 = no retrig. When > 0, `retrig_count` additional hits fire after
    // the primary hit, `retrig_rate_ticks` apart, clipped at the next step
    // boundary (a retrig that would land at or after the next step's
    // trigger tick is dropped rather than overlapping it).
    uint8_t retrig_count = 0;
    uint8_t retrig_rate_ticks = 0;
    ParamLock param_locks[kMaxParamLocks];
};

struct TrackSteps {
    bool enabled = true;  // mute
    Step steps[kMaxSteps];
};

// length/scale/swing are pattern-level (sequencer.md §3): every track in a
// pattern shares the same step grid, only per-step content differs.
struct Pattern {
    uint8_t length = 16;  // 1..kMaxSteps steps; scheduler clamps defensively
    StepScale scale = StepScale::Sixteenth;
    // 50 = straight (no swing) .. 75 = maximum (approaches triplet feel).
    // Applied to odd-indexed steps only (the classic "delay every other
    // 16th" swing model).
    uint8_t swing = 50;
    TrackSteps tracks[kMaxTracks];
};

// One scheduled trigger, sample-accurate (docs/features/sequencer.md §2:
// "a step scheduled at sample 17 of block N starts rendering at exactly
// that frame"). `frame` is the absolute audio-frame count since
// SequencerScheduler::Start(), suitable for handing directly to a voice
// trigger queue alongside the current block's frame range.
struct TriggerEvent {
    uint64_t frame = 0;
    uint8_t track = 0;
    uint8_t step = 0;
    uint8_t velocity = 0;
    bool is_retrig = false;  // false = the step's primary hit, true = a retrig repeat
    uint8_t param_lock_count = 0;
    ParamLock param_locks[kMaxParamLocks];
};

}  // namespace Sequencer
}  // namespace WaveX
