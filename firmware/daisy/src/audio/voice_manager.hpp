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
// plays which sample) - item 8's mapping lives in audio_engine.cpp's
// OnNoteOn, which resolves a loaded sample and feeds Trigger() through an
// SPSC event queue drained by the audio callback; CMSIS-DSP
// interpolation kernels (arm_linear_interp_q15) - the roadmap cites this as
// "(exists)" in the codebase for later use, not something this pass must
// adopt; swapping the portable float interpolation below for a q15 CMSIS
// kernel is a profiling-driven ARM-only optimization (AGENTS.md: "measure
// before/after with the DWT cycle counter... don't assert a performance win
// without a number" - not possible without real hardware), and doing so
// would cost host-testability, which this class currently has. The "2
// concurrent streamed voices with prebuffer admission control" half of
// item 2's text is a separate refactor of the existing singleton
// WAV-streaming path in audio_engine.cpp, tracked separately.
//
// Real-time-safety: Trigger()/Release()/Render() are all callback-safe -
// fixed-size array, no heap allocation, no blocking I/O, no logging
// (AGENTS.md constraint #1 / architecture.md §7.1).

#include "envelope.hpp"
#include "fade.hpp"
#include "svf_filter.hpp"
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
    const int16_t* sample = nullptr;  // RAM-resident, interleaved; not owned by Voice
    uint32_t sample_frames = 0;
    uint8_t src_channels = 1;  // interleave stride: 1 = mono, 2 = stereo (averaged to mono)
    float phase = 0.0f;        // fractional playback position, in frames
    float increment = 1.0f;    // playback rate (pitch), from note/root_note
    // The increment this voice's own note implies, before any live transpose.
    // Kept so ApplyLiveParams can re-apply a pitch offset without losing key
    // tracking - recomputing from `increment` would compound each edit.
    float base_increment = 1.0f;
    float gain = 0.0f;        // 0..1, derived from velocity (× gain_mul)
    float pan = 0.5f;         // 0=left, 1=right, linear (not equal-power)
    uint8_t note = 0;         // MIDI note that triggered this voice
    uint8_t slot = 0;         // instrument slot (kit/multitimbral) that owns this voice
    uint8_t choke_group = 0;  // 0 = none; 1..N = mutual-exclusion group (open/closed hat)
    uint32_t age = 0;         // trigger order, for stealing/release-newest-first

    // Playback region + loop (item 4). end_frame/loop_end are exclusive.
    uint32_t start_frame = 0;
    uint32_t end_frame = 0;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    // Region fades (roadmap 1.5.6 item 3), in frames of the SOURCE sample.
    // Separate from the ADSR below and multiplied with it: the ADSR belongs to
    // the instrument (how this note is played), the fade belongs to the sample
    // (where its region was cut). Folding one into the other would make a
    // marker move change the envelope, or an envelope change move the de-click.
    uint32_t fade_in_frames = 0;
    uint32_t fade_out_frames = 0;

    SvfFilter filter;
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
    // Interleaved channel count of `sample` (1 or 2). A stereo source is
    // averaged to mono before the per-voice filter/envelope/pan chain -
    // voices are mono-in by design (pan re-places them in the stereo
    // field). Loaded WAVs are stored raw/interleaved (see audio_engine
    // OnSampleLoad), so item 8's note path needs this to play 16-bit
    // stereo files without a load-time downmix pass.
    uint8_t channels = 1;
    uint8_t note = 60;
    uint8_t velocity = 127;
    float pan = 0.5f;
    uint8_t root_note = 60;  // note at which `sample` plays at its recorded pitch

    // Instrument-model routing (instrument-model.md §3). slot identifies the
    // owning instrument (for StopSlot on rebind); choke_group != 0 mutes
    // other voices in the same group at trigger time (open/closed hat).
    uint8_t slot = 0;
    uint8_t choke_group = 0;

    // Post-resolution multipliers the instrument layer folds in without
    // re-deriving the base velocity/pitch: gain_mul scales the velocity gain
    // (zone gain, mixer trim, gain modulation); pitch_ratio_mul multiplies
    // the note/root pitch ratio (coarse/fine tune, tuning tables, pitch
    // modulation). Both default to 1.0 (identity) so the plain MIDI path is
    // unchanged.
    float gain_mul = 1.0f;
    float pitch_ratio_mul = 1.0f;

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
    // Region fades in milliseconds, at the sample's own rate. Converted to
    // frames at Trigger() so Render() does no division per block.
    uint16_t fade_in_ms = 0;
    uint16_t fade_out_ms = 0;

    float filter_cutoff_hz = 20000.0f;  // effectively open by default
    // 0 = no resonance, 1 = strongly resonant (svf_filter.hpp maps this onto
    // Q). Defaults to 0 so an unset trigger sounds like the plain lowpass the
    // one-pole used to give - adding the SVF must not put a peak on every
    // voice that never asked for one.
    float filter_resonance = 0.0f;

    float attack_s = 0.001f;
    float decay_s = 0.05f;
    float sustain_level = 0.8f;
    float release_s = 0.1f;
};

// Live (base) voice parameters - the values a knob edits, as opposed to the
// per-trigger snapshot VoiceTriggerParams carries.
//
// These exist because the per-voice filter and envelope were previously
// written ONCE, at Trigger() time, so nothing could change a sounding voice
// and nothing carried an edit forward to the next note. On the all-digital
// path that meant a filter or envelope knob did nothing at all
// (features/digital-voice-audition.md stage 1).
//
// Defaults deliberately match VoiceTriggerParams field for field, so an
// engine that never applies a live edit behaves exactly as before.
struct VoiceLiveParams {
    float filter_cutoff_hz = 20000.0f;
    float filter_resonance = 0.0f;
    // Sample-stage controls. 0.5 is centre; a semitone offset of 0 leaves the
    // sample at its recorded pitch, so an untouched voice is unchanged.
    float pan = 0.5f;
    float pitch_semitones = 0.0f;
    float attack_s = 0.001f;
    float decay_s = 0.05f;
    float sustain_level = 0.8f;
    float release_s = 0.1f;
};

class VoiceManager {
   public:
    // MUST establish every non-zero default this class declares, not just the
    // sample rate. The engine's instance lives in .dtcmram_bss, which is
    // (NOLOAD) and is zeroed at startup: no constructor and no default member
    // initializer ever runs on it, so an initializer written at the member is
    // documentation rather than behaviour. Init() is the only thing that
    // actually sets anything.
    //
    // This is not hypothetical. live_pitch_scale_ was added with `= 1.0f` and
    // no line here, so it was 0 on hardware, every Trigger() computed
    // increment = base_increment * 0, the phase never advanced, and every voice
    // froze on a single sample - silencing both the keyboard and the sample
    // edit page's audition, which also triggers a RAM voice.
    //
    // Anything added below with a non-zero default needs a line here.
    // InitEstablishesDefaultsFromZeroedMemory pins that.
    void Init(uint32_t sample_rate) {
        sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
        live_pitch_scale_ = 1.0f;
    }

    // Pushes live parameter edits onto every SOUNDING voice, so a filter
    // sweep is audible on notes that are already playing rather than only on
    // the next one. Callback-safe: fixed iteration, no allocation, no I/O.
    //
    // Intended to be driven at block rate from the audio callback, and only
    // when something actually changed - see the caller's dirty flag. It is
    // cheap but not free: SetCutoff() recomputes coefficients (a tan()) per
    // voice. Every voice is handed the SAME cutoff and resonance here, so if
    // this ever shows up in a DWT profile the fix is to compute the
    // coefficients once and share them, not to update less often.
    //
    // Envelope rates are deliberately NOT written to a voice that is already
    // releasing. Choke() forces a short release onto a voice immediately
    // before Release() (open/closed hat), and rewriting the ADSR here would
    // hand that voice its full-length release back mid-choke - the hat would
    // not cut off. Filter changes still apply to releasing voices, because a
    // sweep should stay audible through the release tail.
    void ApplyLiveParams(const VoiceLiveParams& p) {
        // One pow() per call, not one per voice: this runs at block rate from
        // the audio callback whenever a control moved, and eight of them would
        // be eight transcendentals inside the deadline for no benefit.
        live_pitch_scale_ = std::pow(2.0f, p.pitch_semitones / 12.0f);
        for (auto& v: voices_) {
            if (v.state != VoiceState::Playing)
                continue;
            v.filter.SetResonance(p.filter_resonance);
            v.filter.SetCutoff(p.filter_cutoff_hz);
            if (!v.envelope.IsReleasing()) {
                v.envelope.SetParams(p.attack_s, p.decay_s, p.sustain_level, p.release_s);
            }
            // Pan is a gain pair recomputed per block in Render(), so writing
            // it here is heard on the next block without a click.
            v.pan = p.pan;
            // Multiply the note's own increment rather than overwrite it, so a
            // live transpose stacks on key tracking instead of flattening every
            // voice to the same rate.
            v.increment = v.base_increment * live_pitch_scale_;
        }
    }

    // Triggers a new voice, or steals one if all 8 are busy (prefers a
    // voice already in its release tail, else the oldest-triggered - see
    // FindVoiceToSteal()). No-op if `params.sample` is null or
    // `params.sample_frames < 2` (can't interpolate).
    void Trigger(const VoiceTriggerParams& params) {
        if (!params.sample || params.sample_frames < 2)
            return;
        // Choke: mute other voices in the same group before allocating this
        // one (the new voice must not choke itself). No-op for group 0.
        if (params.choke_group != 0)
            Choke(params.choke_group);
        int idx = FindFreeVoice();
        if (idx < 0)
            idx = FindVoiceToSteal();
        Voice& v = voices_[static_cast<size_t>(idx)];

        v.state = VoiceState::Playing;
        v.sample = params.sample;
        v.sample_frames = params.sample_frames;
        v.src_channels = (params.channels == 2) ? 2 : 1;
        v.gain = (static_cast<float>(params.velocity) / 127.0f) * params.gain_mul;
        v.pan = params.pan < 0.0f ? 0.0f : (params.pan > 1.0f ? 1.0f : params.pan);
        v.note = params.note;
        v.slot = params.slot;
        v.choke_group = params.choke_group;
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

        // Fades count in source frames, so they use the sample's own rate -
        // not the engine's. A 44.1 kHz sample on a 48 kHz engine advances
        // 0.919 source frames per output frame, and using the engine rate here
        // would make the ramp 9% short in source terms, i.e. it would end
        // before the region boundary it exists to cover.
        const uint32_t src_rate = params.sample_rate_hz ? params.sample_rate_hz : sample_rate_;
        v.fade_in_frames = FadeFrames(params.fade_in_ms, src_rate);
        v.fade_out_frames = FadeFrames(params.fade_out_ms, src_rate);

        // Pitch: 12-TET ratio relative to the sample's recorded root note,
        // times native-rate/engine-rate compensation (a 44.1kHz sample on a
        // 48kHz engine advances 0.919 source frames per output frame so it
        // plays at recorded pitch).
        const float rate_ratio =
            (params.sample_rate_hz > 0)
                ? static_cast<float>(params.sample_rate_hz) / static_cast<float>(sample_rate_)
                : 1.0f;
        v.base_increment = rate_ratio * params.pitch_ratio_mul *
                           std::pow(2.0f,
                                    static_cast<float>(static_cast<int>(params.note) -
                                                       static_cast<int>(params.root_note)) /
                                        12.0f);
        v.increment = v.base_increment * live_pitch_scale_;

        v.filter.Init(sample_rate_);
        v.filter.SetResonance(params.filter_resonance);
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
                bool holding_release_tail = false;
                if (v.loop && v.phase >= last_valid_loop_phase) {
                    v.phase = static_cast<float>(v.loop_start);
                } else if (!v.loop && v.phase >= last_valid_phase) {
                    // Reached the end of a non-looping sample: start the
                    // release tail (or, if already releasing, this just
                    // confirms we're done - the envelope-idle check below
                    // frees the voice). Freeze the read position at the
                    // region boundary instead of continuing to advance
                    // phase - for a trimmed sample (end_frame <
                    // sample_frames) letting phase run on would read
                    // whatever raw audio follows the trim point for the
                    // whole release time.
                    v.envelope.Release();
                    holding_release_tail = true;
                }

                uint32_t idx0, idx1;
                float frac;
                if (holding_release_tail) {
                    idx0 = static_cast<uint32_t>(last_valid_phase);
                    idx1 = idx0 + 1;
                    frac = 0.0f;
                } else {
                    idx0 = static_cast<uint32_t>(v.phase);
                    if (idx0 >= v.sample_frames - 1)
                        idx0 =
                            v.sample_frames - 2;  // clamp: envelope release masks the tail anyway
                    idx1 = idx0 + 1;
                    frac = v.phase - static_cast<float>(idx0);
                }
                float s0, s1;
                if (v.src_channels == 2) {
                    // Interleaved stereo source: average L/R to mono.
                    s0 = (static_cast<float>(v.sample[idx0 * 2]) +
                          static_cast<float>(v.sample[idx0 * 2 + 1])) *
                         0.5f / 32768.0f;
                    s1 = (static_cast<float>(v.sample[idx1 * 2]) +
                          static_cast<float>(v.sample[idx1 * 2 + 1])) *
                         0.5f / 32768.0f;
                } else {
                    s0 = static_cast<float>(v.sample[idx0]) / 32768.0f;
                    s1 = static_cast<float>(v.sample[idx1]) / 32768.0f;
                }
                float s = s0 + (s1 - s0) * frac;

                s = v.filter.Process(s);
                float env = v.envelope.Process();
                s *= env;
                if (v.fade_in_frames != 0 || v.fade_out_frames != 0) {
                    s *= RegionFadeGain(
                        idx0, v.start_frame, v.end_frame, v.fade_in_frames, v.fade_out_frames);
                }

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

    // Hard-stops every voice immediately (no release tail). For sample-
    // memory invalidation: audio_engine calls this (from the audio
    // callback, via a flag set by the main loop) before a loaded sample's
    // backing memory is released/rewritten, so no voice keeps reading
    // freed SDRAM. Callback-safe: just clears state.
    void StopAll() {
        for (auto& v: voices_) {
            v.state = VoiceState::Idle;
        }
    }

    // Choke group (instrument-model.md §3): forces every playing voice in
    // `group` that is not already releasing into a fast release, so the
    // classic open-hat/closed-hat mutual exclusion cuts the open hat off
    // near-instantly but click-free. `group` 0 is "no group" and never
    // chokes anything. Called from Trigger() before allocating the new
    // voice; callback-safe (no alloc/IO). fast_release_s default 5 ms.
    void Choke(uint8_t group, float fast_release_s = 0.005f) {
        if (group == 0)
            return;
        for (auto& v: voices_) {
            if (v.state == VoiceState::Playing && v.choke_group == group &&
                !v.envelope.IsReleasing()) {
                v.envelope.SetReleaseTime(fast_release_s);
                v.envelope.Release();
            }
        }
    }

    // Hard-stops (no release tail) only voices owned by `slot`. Used when an
    // instrument slot is rebound to a new instrument whose sample memory is
    // about to be released - scoped so rebinding slot 3 doesn't cut off
    // slots 0-2 (instrument-model.md §4). Callback-safe.
    void StopSlot(uint8_t slot) {
        for (auto& v: voices_) {
            if (v.slot == slot)
                v.state = VoiceState::Idle;
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

    // Voices that are sounding and NOT yet in their release tail - the
    // "held" gate for the Stage A paraphonic envelope law (a non-looping
    // sample reaching its end auto-releases and stops counting; see
    // paraphonic_envelope.hpp).
    uint8_t HeldVoiceCount() const {
        uint8_t count = 0;
        for (const auto& v: voices_) {
            if (v.state == VoiceState::Playing && !v.envelope.IsReleasing())
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
    // Live transpose as a rate multiplier. 1.0 until something moves PARAM_PITCH,
    // so a voice triggered before any edit sounds exactly as it did before.
    float live_pitch_scale_ = 1.0f;
};

}  // namespace AudioEngine
}  // namespace WaveX
