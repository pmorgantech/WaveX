#pragma once

// 8-voice polyphonic RAM-resident sample player (roadmap Phase 1 items 2 +
// 4). HAL-free: operates purely on int16_t* sample data (owned elsewhere -
// the SDRAM slab/extent allocator in memory.h, SampleMemMgr) and float
// output buffers, so it's host-testable without any Daisy hardware or
// cross compiler.
//
// In scope here: voice allocation/stealing across 8 voices, per-voice
// gain/pan/pitch (note-relative-to-root-note ratio), start/end/loop points,
// a one-pole digital filter stand-in for the analog VCF (item 4), a linear
// ADSR envelope per voice (item 4), RAM-resident triggering with zero I/O
// (Trigger() just stores state - no allocation, no blocking), and
// rendering into the stereo buffers the output sink (output_sink.hpp,
// item 1) consumes.
//
// Deliberately NOT in scope: note-to-sample mapping policy (which MIDI note
// plays which sample - item 8, "MIDI note path ... -> voice manager", wires
// that up once a kit/pad-mapping concept exists in Phase 2); CMSIS-DSP
// interpolation kernels (arm_linear_interp_q15) - the roadmap cites this as
// "(exists)" in the codebase for later use, not something this pass must
// adopt; swapping the portable float interpolation below for a q15 CMSIS
// kernel is a profiling-driven ARM-only optimization (AGENTS.md: "measure
// before/after with the DWT cycle counter... don't assert a performance win
// without a number" - not possible without real hardware), and doing so
// would cost host-testability, which this class currently has. The "2
// concurrent streamed voices with prebuffer admission control" half of
// item 2's text is a separate refactor of the existing singleton
// WAV-streaming path in audio_engine.cpp, tracked separately. Not yet wired
// into audio_engine.cpp's Callback() - nothing safe to verify on real
// hardware should be wired into the audio callback without something
// meaningful (a real note-to-sample trigger source, item 8) actually
// driving it.
//
// Real-time-safety: Trigger()/Release()/Render() are all callback-safe -
// fixed-size array, no heap allocation, no blocking I/O, no logging
// (AGENTS.md constraint #1 / architecture.md §7.1).

#include "envelope.hpp"
#include "one_pole_filter.hpp"
#include <array>
#include <cmath>
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
    float increment = 1.0f;  // playback rate (pitch), from note/root_note
    float gain = 0.0f;       // 0..1, derived from velocity
    float pan = 0.5f;        // 0=left, 1=right, linear (not equal-power)
    uint8_t note = 0;        // MIDI note that triggered this voice
    uint32_t age = 0;        // trigger order, for stealing/release-newest-first

    // Playback region + loop (item 4). end_frame/loop_end are exclusive.
    uint32_t start_frame = 0;
    uint32_t end_frame = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;

    OnePoleFilter filter;
    Envelope envelope;

    bool IsFree() const { return state == VoiceState::Idle; }
};

// Named-argument trigger parameters (roadmap-item-4-sized Trigger() calls
// don't fit sanely as positional arguments - see the wire-struct-hygiene
// precedent elsewhere in this codebase for why named fields over positional
// sprawl). Only `sample`/`sample_frames` are required; everything else has
// a sensible default for a plain one-shot voice.
struct VoiceTriggerParams {
    const int16_t* sample = nullptr;
    uint32_t sample_frames = 0;
    uint8_t note = 60;
    uint8_t velocity = 127;
    float pan = 0.5f;
    uint8_t root_note = 60;  // note at which `sample` plays at its recorded pitch

    // The sample's native rate in Hz. 0 (default) means "same as the
    // engine" - no compensation. When set (e.g. 44100 for a 44.1kHz WAV on
    // the 48kHz engine), playback rate is scaled by native/engine so the
    // sample plays at its recorded pitch without resampling its data -
    // the fractional-phase linear interpolation in Render() does the work
    // (dma-timing-review-2026-07-03.md Finding 1, "playback-rate
    // compensation" option).
    uint32_t sample_rate_hz = 0;

    uint32_t start_frame = 0;
    uint32_t end_frame = 0;  // 0 => sample_frames
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;  // 0 => end_frame

    float filter_cutoff_hz = 20000.0f;  // effectively open/bypass by default

    float attack_s = 0.001f;
    float decay_s = 0.05f;
    float sustain_level = 0.8f;
    float release_s = 0.1f;
};

class VoiceManager {
   public:
    void Init(uint32_t sample_rate) { sample_rate_ = sample_rate > 0 ? sample_rate : 48000; }

    // Triggers a new voice, or steals one if all 8 are busy (prefers a
    // voice already in its release tail, else the oldest-triggered - see
    // FindVoiceToSteal()). No-op if `params.sample` is null or
    // `params.sample_frames < 2` (can't interpolate).
    void Trigger(const VoiceTriggerParams& params) {
        if (!params.sample || params.sample_frames < 2)
            return;
        int idx = FindFreeVoice();
        if (idx < 0)
            idx = FindVoiceToSteal();
        Voice& v = voices_[static_cast<size_t>(idx)];

        v.state = VoiceState::Playing;
        v.sample = params.sample;
        v.sample_frames = params.sample_frames;
        v.gain = static_cast<float>(params.velocity) / 127.0f;
        v.pan = params.pan < 0.0f ? 0.0f : (params.pan > 1.0f ? 1.0f : params.pan);
        v.note = params.note;
        v.age = next_age_++;

        v.start_frame = params.start_frame < params.sample_frames ? params.start_frame : 0;
        v.end_frame = (params.end_frame == 0 || params.end_frame > params.sample_frames)
                          ? params.sample_frames
                          : params.end_frame;
        v.loop = params.loop;
        v.loop_start = params.loop_start < v.end_frame ? params.loop_start : v.start_frame;
        v.loop_end =
            (params.loop_end == 0 || params.loop_end > v.end_frame) ? v.end_frame : params.loop_end;
        v.phase = static_cast<float>(v.start_frame);

        // Pitch: 12-TET ratio relative to the sample's recorded root note,
        // times native-rate/engine-rate compensation (a 44.1kHz sample on a
        // 48kHz engine advances 0.919 source frames per output frame so it
        // plays at recorded pitch).
        const float rate_ratio =
            (params.sample_rate_hz > 0)
                ? static_cast<float>(params.sample_rate_hz) / static_cast<float>(sample_rate_)
                : 1.0f;
        v.increment = rate_ratio * std::pow(2.0f,
                                            static_cast<float>(static_cast<int>(params.note) -
                                                               static_cast<int>(params.root_note)) /
                                                12.0f);

        v.filter.Init(sample_rate_);
        v.filter.SetCutoff(params.filter_cutoff_hz);
        v.filter.Reset();

        v.envelope.Init(sample_rate_);
        v.envelope.SetParams(
            params.attack_s, params.decay_s, params.sustain_level, params.release_s);
        v.envelope.Retrigger();
    }

    // Starts the release phase of the most recently triggered still-active
    // voice for `note` (envelope decays over its release time - the voice
    // stays allocated/rendering until the envelope reaches silence, it does
    // not stop immediately). No-op if no voice is active for that note.
    void Release(uint8_t note) {
        int found = -1;
        uint32_t newest_age = 0;
        for (uint8_t i = 0; i < kNumVoices; ++i) {
            if (voices_[i].state == VoiceState::Playing && voices_[i].note == note &&
                !voices_[i].envelope.IsReleasing()) {
                if (found < 0 || voices_[i].age >= newest_age) {
                    found = i;
                    newest_age = voices_[i].age;
                }
            }
        }
        if (found >= 0) {
            voices_[static_cast<size_t>(found)].envelope.Release();
        }
    }

    // Renders one audio block: clears out_l/out_r, then sums every active
    // voice's linearly-interpolated, filtered, ADSR- and gain/pan-scaled
    // contribution. A voice frees its slot once its envelope reaches
    // silence (release complete) or - for a non-looping voice - once
    // playback reaches end_frame with the envelope not yet started
    // releasing (treated as an implicit release-from-here).
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
            const float last_valid_phase = static_cast<float>(v.end_frame - 1);
            const float last_valid_loop_phase = static_cast<float>(v.loop_end - 1);

            for (size_t i = 0; i < block_size; ++i) {
                if (v.loop && v.phase >= last_valid_loop_phase) {
                    v.phase = static_cast<float>(v.loop_start);
                } else if (!v.loop && v.phase >= last_valid_phase) {
                    // Reached the end of a non-looping sample: start the
                    // release tail (or, if already releasing, this just
                    // confirms we're done - the envelope-idle check below
                    // frees the voice).
                    v.envelope.Release();
                }

                uint32_t idx0 = static_cast<uint32_t>(v.phase);
                if (idx0 >= v.sample_frames - 1)
                    idx0 = v.sample_frames - 2;  // clamp: envelope release masks the tail anyway
                uint32_t idx1 = idx0 + 1;
                float frac = v.phase - static_cast<float>(idx0);
                float s0 = static_cast<float>(v.sample[idx0]) / 32768.0f;
                float s1 = static_cast<float>(v.sample[idx1]) / 32768.0f;
                float s = s0 + (s1 - s0) * frac;

                s = v.filter.Process(s);
                float env = v.envelope.Process();
                s *= env;

                out_l[i] += s * left_gain;
                out_r[i] += s * right_gain;

                if (v.envelope.IsIdle()) {
                    v.state = VoiceState::Idle;
                    break;
                }

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

    // Prefers stealing a voice already in its release tail (least
    // perceptually disruptive); falls back to the oldest-triggered voice
    // if none are releasing.
    int FindVoiceToSteal() const {
        int releasing_oldest = -1;
        uint8_t overall_oldest = 0;
        for (uint8_t i = 0; i < kNumVoices; ++i) {
            if (voices_[i].age < voices_[overall_oldest].age)
                overall_oldest = i;
            if (voices_[i].envelope.IsReleasing() &&
                (releasing_oldest < 0 ||
                 voices_[i].age < voices_[static_cast<size_t>(releasing_oldest)].age)) {
                releasing_oldest = i;
            }
        }
        return releasing_oldest >= 0 ? releasing_oldest : overall_oldest;
    }

    std::array<Voice, kNumVoices> voices_{};
    uint32_t next_age_ = 0;
    uint32_t sample_rate_ = 48000;
};

}  // namespace AudioEngine
}  // namespace WaveX
