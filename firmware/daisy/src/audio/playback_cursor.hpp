#pragma once

#include "voice_manager.hpp"
#include <cstdint>

namespace WaveX::AudioEngine {

// Display metadata only. The producer walks the same local float phase as
// the streaming resampler, retaining an integer base for long-file accuracy.
// A negative phase reads the previous chunk's history, which can precede a
// loop rewind and therefore need not be first_frame - 1.
struct SourceFrameWalk {
    static constexpr uint32_t kSilent = UINT32_MAX;
    uint32_t first_frame = 0;
    uint32_t history_frame = kSilent;
    float phase = 0;
    float step = 1;
    bool silent = true;

    uint32_t Next() {
        if (silent)
            return kSilent;
        const uint32_t frame =
            phase < 0 ? history_frame : first_frame + static_cast<uint32_t>(phase);
        phase += step;
        return frame;
    }
};

// Callback-owned voices only. No PCM reads: the pointer is an identity token,
// and foreground revalidates the Pool generation before sending the reply.
inline bool FindVoicePlayhead(const VoiceManager& manager, const int16_t* pcm, uint32_t& frame) {
    if (!pcm)
        return false;
    bool found = false;
    uint32_t newest = 0;
    for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
        const auto& voice = manager.GetVoice(i);
        if (voice.IsFree() || (found && static_cast<int32_t>(voice.age - newest) <= 0))
            continue;
        const VoiceSampleState* source = voice.sample == pcm             ? &voice
                                         : voice.secondary.sample == pcm ? &voice.secondary
                                                                         : nullptr;
        if (!source || !source->sample_frames || !source->end_frame)
            continue;
        uint32_t position = source->phase.Frame();
        if (source->loop && position >= source->loop_end) {
            position -= source->loop_end - source->loop_start;
            if (position >= source->loop_end || position < source->loop_start)
                position = source->loop_start;
        }
        frame = position < source->end_frame ? position : source->end_frame - 1;
        newest = voice.age;
        found = true;
    }
    return found;
}

}  // namespace WaveX::AudioEngine
