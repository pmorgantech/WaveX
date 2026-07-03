#pragma once

// 8-voice polyphonic RAM-resident sample player (roadmap Phase 1 item 2).
// HAL-free: operates purely on int16_t* sample data (owned elsewhere - the
// SDRAM slab/extent allocator in memory.h, SampleMemMgr) and float output
// buffers, so it's host-testable without any Daisy hardware or cross
// compiler.
//
// In scope here: voice allocation/stealing across 8 voices, per-voice
// gain/pan/pitch, RAM-resident triggering with zero I/O (Trigger() just
// stores a pointer + resets phase - no allocation, no blocking), and
// rendering into the stereo buffers the output sink (output_sink.hpp,
// Phase 1 item 1) consumes.
//
// Deliberately NOT in scope: note-to-sample mapping policy (which MIDI note
// plays which sample - item 8, "MIDI note path ... -> voice manager", wires
// that up once a kit/pad-mapping concept exists in Phase 2); interpolated
// pitch via CMSIS-DSP kernels, loop points, filter, ADSR (item 4 - this
// class does plain linear interpolation and hard-stops at sample end, no
// looping); the "2 concurrent streamed voices with prebuffer admission
// control" half of item 2's text, which is a separate refactor of the
// existing singleton WAV-streaming path in audio_engine.cpp, tracked
// separately. Not yet wired into audio_engine.cpp's Callback() - same
// reasoning as the output/CV seam: nothing safe to verify on real hardware
// should be wired into the audio callback without something meaningful (a
// real note-to-sample trigger source) actually driving it.
//
// Real-time-safety: Trigger()/Release()/Render() are all callback-safe -
// fixed-size array, no heap allocation, no blocking I/O, no logging
// (AGENTS.md constraint #1 / architecture.md §7.1).

#include <array>
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

static constexpr uint8_t kNumVoices = 8;

enum class VoiceState : uint8_t { Idle, Playing };

struct Voice {
    VoiceState state = VoiceState::Idle;
    const int16_t* sample = nullptr;  // mono, RAM-resident; not owned by Voice
    uint32_t sample_frames = 0;
    float phase = 0.0f;      // fractional playback position, in frames
    float increment = 1.0f;  // playback rate (pitch); item 4 sets this from note number
    float gain = 0.0f;       // 0..1, derived from velocity
    float pan = 0.5f;        // 0=left, 1=right, linear (not equal-power)
    uint8_t note = 0;        // MIDI note that triggered this voice
    uint32_t age = 0;        // trigger order, for stealing/release-newest-first

    bool IsFree() const { return state == VoiceState::Idle; }
};

class VoiceManager {
   public:
    // Trigger a new voice playing `sample` (mono, RAM-resident, `frames`
    // long, minimum 2 for interpolation) at the given note/velocity.
    // Allocates a free voice, or steals the oldest-triggered voice if all 8
    // are busy. No-op if `sample` is null or `frames < 2`.
    void Trigger(
        const int16_t* sample, uint32_t frames, uint8_t note, uint8_t velocity, float pan = 0.5f) {
        if (!sample || frames < 2)
            return;
        int idx = FindFreeVoice();
        if (idx < 0)
            idx = FindVoiceToSteal();
        Voice& v = voices_[static_cast<size_t>(idx)];
        v.state = VoiceState::Playing;
        v.sample = sample;
        v.sample_frames = frames;
        v.phase = 0.0f;
        v.increment = 1.0f;  // pitch-from-note mapping is item 4's job
        v.gain = static_cast<float>(velocity) / 127.0f;
        v.pan = pan < 0.0f ? 0.0f : (pan > 1.0f ? 1.0f : pan);
        v.note = note;
        v.age = next_age_++;
    }

    // Hard-stops the most recently triggered still-playing voice for
    // `note` (no envelope/release tail yet - item 4 adds one). No-op if no
    // voice is playing that note.
    void Release(uint8_t note) {
        int found = -1;
        uint32_t newest_age = 0;
        for (uint8_t i = 0; i < kNumVoices; ++i) {
            if (voices_[i].state == VoiceState::Playing && voices_[i].note == note) {
                if (found < 0 || voices_[i].age >= newest_age) {
                    found = i;
                    newest_age = voices_[i].age;
                }
            }
        }
        if (found >= 0) {
            voices_[static_cast<size_t>(found)].state = VoiceState::Idle;
        }
    }

    // Renders one audio block: clears out_l/out_r, then sums every active
    // voice's linearly-interpolated, gain/pan-scaled contribution. Voices
    // that reach the end of their sample self-stop mid-block (no looping).
    void Render(float* out_l, float* out_r, size_t block_size) {
        for (size_t i = 0; i < block_size; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        for (uint8_t vi = 0; vi < kNumVoices; ++vi) {
            Voice& v = voices_[vi];
            if (v.state != VoiceState::Playing)
                continue;
            const float left_gain = v.gain * (1.0f - v.pan);
            const float right_gain = v.gain * v.pan;
            const float last_valid_phase = static_cast<float>(v.sample_frames - 1);
            for (size_t i = 0; i < block_size; ++i) {
                if (v.phase >= last_valid_phase) {
                    v.state = VoiceState::Idle;
                    break;
                }
                uint32_t idx0 = static_cast<uint32_t>(v.phase);
                uint32_t idx1 = idx0 + 1;
                float frac = v.phase - static_cast<float>(idx0);
                float s0 = static_cast<float>(v.sample[idx0]) / 32768.0f;
                float s1 = static_cast<float>(v.sample[idx1]) / 32768.0f;
                float s = s0 + (s1 - s0) * frac;
                out_l[i] += s * left_gain;
                out_r[i] += s * right_gain;
                v.phase += v.increment;
            }
        }
    }

    uint8_t ActiveVoiceCount() const {
        uint8_t count = 0;
        for (const auto& v: voices_) {
            if (v.state != VoiceState::Idle)
                ++count;
        }
        return count;
    }

    const Voice& GetVoice(uint8_t i) const { return voices_[i]; }

   private:
    int FindFreeVoice() const {
        for (uint8_t i = 0; i < kNumVoices; ++i) {
            if (voices_[i].IsFree())
                return i;
        }
        return -1;
    }

    // Simplest correct stealing policy: evict the oldest-triggered voice.
    // Refining this (e.g. prefer released/quietest voices) is natural
    // follow-up once item 4's ADSR gives voices a release phase to prefer
    // stealing from.
    int FindVoiceToSteal() const {
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < kNumVoices; ++i) {
            if (voices_[i].age < voices_[oldest].age)
                oldest = i;
        }
        return oldest;
    }

    std::array<Voice, kNumVoices> voices_{};
    uint32_t next_age_ = 0;
};

}  // namespace AudioEngine
}  // namespace WaveX
