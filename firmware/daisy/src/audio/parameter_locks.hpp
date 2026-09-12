#pragma once

#include "audio/voice_manager.hpp"
#include "sequencer/parameter_locks.hpp"
#include "sequencer/pattern.hpp"

namespace WaveX {
namespace AudioEngine {
inline void ApplySourcePositionLock(VoiceSampleParams& p, const Sequencer::ParamLock& lock) {
    const uint32_t end =
        p.end_frame && p.end_frame < p.sample_frames ? p.end_frame : p.sample_frames;
    const uint32_t start = p.start_frame < end ? p.start_frame : 0;
    const uint32_t loop_end = p.loop_end && p.loop_end < end ? p.loop_end : end;
    const uint32_t loop_start = p.loop_start < loop_end ? p.loop_start : start;
    if (lock.param_id == Protocol::PARAM_SAMPLE_START && end > start && end - start >= 2)
        p.start_frame = start + static_cast<uint32_t>(
                                    (static_cast<uint64_t>(end - start - 2) * lock.value) / 65535u);
    if (lock.param_id == Protocol::PARAM_LOOP_START && p.loop && loop_end > loop_start &&
        loop_end - loop_start >= 2)
        p.loop_start =
            loop_start +
            static_cast<uint32_t>((static_cast<uint64_t>(loop_end - loop_start - 2) * lock.value) /
                                  65535u);
}
// Callback-only, bounded to four locks. Does not mutate the Instrument,
// prepared map, sample metadata or any other voice. Modulation is applied later.
inline void ApplyParamLocks(VoiceTriggerParams& p, const Sequencer::ParamLock* locks, uint8_t n) {
    using namespace Protocol;
    if (!locks)
        return;
    const auto original = static_cast<const VoiceSampleParams&>(p);
    const auto original_secondary = p.secondary;
    for (uint8_t i = 0; i < n && i < Sequencer::kMaxParamLocks; ++i) {
        const auto& lock = locks[i];
        if (!Sequencer::IsVoiceLockParameter(lock.param_id))
            continue;
        const float norm = static_cast<float>(lock.value) / 65535.0f;
        p.param_lock_mask |= VoiceLockBit(lock.param_id);
        switch (lock.param_id) {
            case PARAM_FILTER_CUTOFF:
                p.filter_cutoff_hz = 20.0f * std::pow(1000.0f, norm);
                break;
            case PARAM_FILTER_RESONANCE:
                p.filter_resonance = norm;
                break;
            case PARAM_ENVELOPE_ATTACK:
                p.attack_s = 0.001f + norm * 2.0f;
                break;
            case PARAM_ENVELOPE_DECAY:
                p.decay_s = 0.001f + norm * 2.0f;
                break;
            case PARAM_ENVELOPE_SUSTAIN:
                p.sustain_level = norm;
                break;
            case PARAM_ENVELOPE_RELEASE:
                p.release_s = 0.001f + norm * 2.0f;
                break;
            case PARAM_PAN:
                p.pan = norm;
                break;
            case PARAM_PITCH:
                p.locked_pitch_scale = std::pow(2.0f, (norm * 2.0f - 1.0f) * 2.0f);
                break;
            case PARAM_GAIN:
                p.gain_mul *= static_cast<float>(lock.value) / 32768.0f;
                break;
            case PARAM_SAMPLE_START:
            case PARAM_LOOP_START: {
                auto primary = original;
                auto secondary = original_secondary;
                ApplySourcePositionLock(primary, lock);
                ApplySourcePositionLock(secondary, lock);
                if (lock.param_id == PARAM_SAMPLE_START) {
                    p.start_frame = primary.start_frame;
                    p.secondary.start_frame = secondary.start_frame;
                } else {
                    p.loop_start = primary.loop_start;
                    p.secondary.loop_start = secondary.loop_start;
                }
            } break;
            default:
                break;
        }
    }
}
}  // namespace AudioEngine
}  // namespace WaveX
