#pragma once

// Output sink split (architecture.md §5.3, roadmap Phase 1 item 1).
//
// Voices always render into per-voice block buffers; a sink stage consumes
// them into the physical output path. StereoMixSink sums into the SAI1
// stereo stream (Stage A, live today). TdmVoiceSink interleaves voice i into
// TDM slot i (Stage B, stub - SAI2/PCM1690 bring-up is roadmap Phase 3).
// Selected at compile time by WAVEX_VOICE_OUTPUT_BACKEND
// (firmware/shared/config/hardware_config.h); nothing above this header may
// branch on that flag.
//
// Per-voice gain/pan/mixing decisions belong to the voice manager (roadmap
// Phase 1 item 2, not yet built) - these sinks only combine whatever the
// caller already staged into voice_buffers. Not yet wired into
// audio_engine.cpp's callback: today's engine has exactly one playback
// source (WAV ring buffer / Sampler), not an array of voices, so there is
// nothing for a sink to consume until the voice manager exists. This header
// exists so that seam is already defined and compiling under both flag sets
// before that retrofit becomes expensive.
//
// Real-time-safety: Process() is intended to be called from the audio
// callback. No blocking I/O, no heap allocation, no logging in any
// implementation here (AGENTS.md constraint #1 / architecture.md §7.1).

#include <cstddef>

namespace WaveX {
namespace AudioEngine {

// One voice's rendered output for a block. `right` may equal `left` (same
// pointer) for a mono voice being played dual-mono.
struct VoiceBuffer {
    const float* left;
    const float* right;
};

// Stage A: sums all voices into the SAI1 stereo stream.
class StereoMixSink {
   public:
    void Process(const VoiceBuffer* voices,
                 size_t voice_count,
                 size_t block_size,
                 float* out_l,
                 float* out_r) const {
        for (size_t i = 0; i < block_size; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        for (size_t v = 0; v < voice_count; ++v) {
            const float* l = voices[v].left;
            const float* r = voices[v].right;
            for (size_t i = 0; i < block_size; ++i) {
                out_l[i] += l[i];
                out_r[i] += r[i];
            }
        }
    }
};

// Stage B stub: interleaves voice i into TDM slot i for the PCM1690 (8
// slots, one per voice). No SAI2 DMA path exists yet; this only defines the
// interface so WAVEX_VOICE_OUTPUT_TDM8 compiles cleanly in CI ahead of the
// Phase 3 hardware bring-up that gives it a real implementation.
class TdmVoiceSink {
   public:
    static constexpr size_t kNumSlots = 8;

    void Process(const VoiceBuffer* voices,
                 size_t voice_count,
                 size_t block_size,
                 float* tdm_slots[kNumSlots]) const {
        (void)voice_count;
        (void)block_size;
        for (size_t slot = 0; slot < kNumSlots; ++slot) {
            if (slot < voice_count && tdm_slots[slot]) {
                // TODO(roadmap Phase 3): route voices[slot].left/right into the
                // real SAI2 TDM DMA buffer instead of a plain copy.
                for (size_t i = 0; i < block_size; ++i) {
                    tdm_slots[slot][i] = voices[slot].left[i];
                }
            }
        }
    }
};

}  // namespace AudioEngine
}  // namespace WaveX
