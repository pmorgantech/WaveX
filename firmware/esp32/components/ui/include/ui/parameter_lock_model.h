#pragma once
#include "sequencer/parameter_locks.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wavex_ui {
namespace ParameterLocks {
using namespace WaveX::Protocol;
struct Choice {
    uint8_t id;
    const char* name;
    uint16_t initial;
};
inline constexpr Choice choices[] = {{0, "EMPTY", 0},
                                     {PARAM_FILTER_CUTOFF, "CUTOFF", 65535},
                                     {PARAM_FILTER_RESONANCE, "RESONANCE", 0},
                                     {PARAM_ENVELOPE_ATTACK, "ATTACK", 33},
                                     {PARAM_ENVELOPE_DECAY, "DECAY", 1606},
                                     {PARAM_ENVELOPE_SUSTAIN, "SUSTAIN", 52428},
                                     {PARAM_ENVELOPE_RELEASE, "RELEASE", 3244},
                                     {PARAM_PAN, "PAN", 32768},
                                     {PARAM_PITCH, "PITCH", 32768},
                                     {PARAM_GAIN, "GAIN", 32768},
                                     {PARAM_SAMPLE_START, "START", 0},
                                     {PARAM_LOOP_START, "LOOP START", 0}};
inline int Index(uint8_t id) {
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i)
        if (choices[i].id == id)
            return static_cast<int>(i);
    return 0;
}
inline const char* Name(uint8_t id) {
    const int index = Index(id);
    return id && !index ? "UNSUPPORTED" : choices[index].name;
}
inline bool Replace(SeqStepState& step, uint8_t slot, uint8_t id, uint16_t value) {
    if (slot >= SEQ_STEP_LOCKS || (id && !WaveX::Sequencer::IsVoiceLockParameter(id)))
        return false;
    for (uint8_t i = 0; i < SEQ_STEP_LOCKS; ++i)
        if (id && i != slot && step.locks[i].parameter == id)
            return false;
    step.locks[slot].parameter = id;
    step.locks[slot].value = id ? value : 0;
    return true;
}
inline uint8_t Next(const SeqStepState& step, uint8_t slot, int direction) {
    if (slot >= SEQ_STEP_LOCKS || !direction)
        return 0;
    constexpr int count = sizeof(choices) / sizeof(choices[0]);
    int index = Index(step.locks[slot].parameter);
    for (int i = 0; i < count; ++i) {
        index = (index + (direction > 0 ? 1 : count - 1)) % count;
        auto copy = step;
        if (Replace(copy, slot, choices[index].id, choices[index].initial))
            return choices[index].id;
    }
    return 0;
}
inline void Format(const SeqLockState& lock, char* value, size_t capacity, const char*& unit) {
    const float norm = static_cast<float>(lock.value) / 65535.0f;
    if (lock.parameter && !WaveX::Sequencer::IsVoiceLockParameter(lock.parameter)) {
        std::snprintf(value, capacity, "%u", lock.value);
        unit = "raw";
        return;
    }
    unit = "%";
    float display = norm * 100;
    switch (lock.parameter) {
        case 0:
            std::snprintf(value, capacity, "--");
            unit = "";
            return;
        case PARAM_FILTER_CUTOFF:
            display = 20 * std::pow(1000.0f, norm);
            unit = "Hz";
            break;
        case PARAM_ENVELOPE_ATTACK:
        case PARAM_ENVELOPE_DECAY:
        case PARAM_ENVELOPE_RELEASE:
            display = 1 + norm * 2000;
            unit = "ms";
            break;
        case PARAM_PITCH:
            std::snprintf(value, capacity, "%+.1f", (norm * 2 - 1) * 24);
            unit = "semitones";
            return;
        case PARAM_GAIN:
            display = static_cast<float>(lock.value) / 32768.0f * 100;
            break;
        default:
            break;
    }
    std::snprintf(value, capacity, "%.0f", display);
}
}  // namespace ParameterLocks
}  // namespace wavex_ui
