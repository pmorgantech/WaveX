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

#include "audio/mod_matrix.hpp"
#include "audio/track_mix.hpp"
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

// Playback position with an exact 32-bit frame index and a Q24 fractional
// component. A single float cannot advance by one frame once it reaches 2^24,
// which is well inside the duration of a mono sample that fits the SDRAM
// arena. Keeping the integer and fraction separate also makes the callback's
// per-sample advance two integer adds rather than a software double operation.
class PlaybackPhase {
   public:
    static constexpr uint32_t kFractionOne = 16777216u;

    void SetFrame(uint32_t frame) {
        frame_ = frame;
        fraction_ = 0;
    }

    uint32_t Frame() const { return frame_; }
    float Fraction() const { return static_cast<float>(fraction_) * (1.0f / 16777216.0f); }

    void Advance(uint32_t whole, uint32_t fraction) {
        const uint32_t accumulated = fraction_ + fraction;
        frame_ += whole + (accumulated / kFractionOne);
        fraction_ = accumulated % kFractionOne;
    }

    void SubtractFrames(uint32_t frames) { frame_ = frame_ >= frames ? frame_ - frames : 0; }

    static void SplitRate(float rate, uint32_t& whole, uint32_t& fraction) {
        if (!(rate > 0.0f) || !std::isfinite(rate)) {
            whole = 0;
            fraction = 0;
            return;
        }
        // UINT32_MAX itself rounds to 2^32 in binary32, so clamp before the
        // float-to-uint32 conversion can leave the representable range.
        if (rate >= 4294967040.0f) {
            whole = 0xFFFFFFFFu;
            fraction = 0;
            return;
        }
        whole = static_cast<uint32_t>(rate);
        const float fractional = rate - static_cast<float>(whole);
        fraction = static_cast<uint32_t>(fractional * static_cast<float>(kFractionOne) + 0.5f);
        if (fraction >= kFractionOne) {
            ++whole;
            fraction = 0;
        }
    }

   private:
    uint32_t frame_ = 0;
    uint32_t fraction_ = 0;
};

struct Voice {
    VoiceState state = VoiceState::Idle;
    const int16_t* sample = nullptr;  // RAM-resident, interleaved; not owned by Voice
    uint32_t sample_frames = 0;
    uint8_t src_channels = 1;  // interleave stride: 1 = mono, 2 = stereo (averaged to mono)
    PlaybackPhase phase;       // exact frame + fractional playback position
    float increment = 1.0f;    // playback rate (pitch), from note/root_note
    uint32_t increment_frames = 1;
    uint32_t increment_fraction = 0;
    // The increment this voice's own note implies, before any live transpose.
    // Kept so ApplyLiveParams can re-apply a pitch offset without losing key
    // tracking - recomputing from `increment` would compound each edit.
    float base_increment = 1.0f;
    float gain = 0.0f;        // 0..1, derived from velocity (× gain_mul)
    float pan = 0.5f;         // 0=left, 1=right, linear (not equal-power)
    uint8_t note = 0;         // MIDI note that triggered this voice
    uint8_t slot = 0;         // instrument slot (kit/multitimbral) that owns this voice
    uint8_t choke_group = 0;  // 0 = none; 1..N = mutual-exclusion group (open/closed hat)
    bool one_shot = false;    // ignore note-off; stop at the sample/region end
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
    // Second envelope (param-locks-and-modulation.md §4), exposed only as
    // SRC_ENV_FILTER. Unlike `envelope` above, this is a MODULATION SOURCE,
    // not audio: nothing reads it between control ticks, so it advances via
    // AdvanceBlock() once per block from TickModulation() rather than
    // Process() once per sample - the same audible output either way, at a
    // fraction of the cost.
    Envelope env2;
    // The cutoff Trigger()/ApplyLiveParams last set, before modulation. Kept
    // so the control tick can recompute filter.SetCutoff(base * mod_cutoff_mul)
    // every block without compounding onto the previous block's modulated
    // value - the same reason base_increment exists for pitch.
    float base_cutoff_hz = 20000.0f;

    // Block-rate modulation (param-locks-and-modulation.md §3), written once
    // per control tick by SetBlockModulation() and consumed at the top of
    // this voice's Render() slice - never per sample. Multipliers/offset
    // rather than absolute values so modulation composes onto whatever the
    // zone and live params already set. Identity by default, so a voice
    // nothing modulates renders exactly as it did before the matrix existed.
    float mod_cutoff_mul = 1.0f;
    float mod_gain_mul = 1.0f;
    float mod_pitch_mul = 1.0f;
    float mod_pan_offset = 0.0f;

    // Per-trigger modulation sources (SRC_VELOCITY/SRC_NOTE/SRC_RANDOM),
    // sampled once at Trigger() and held constant for the voice's lifetime -
    // §3 requires these NOT be re-sampled every control tick.
    float mod_velocity = 0.0f;
    float mod_note = 0.0f;
    float mod_random = 0.0f;

    bool IsFree() const { return state == VoiceState::Idle; }

    void SetIncrement(float rate) {
        increment = rate;
        PlaybackPhase::SplitRate(rate, increment_frames, increment_fraction);
    }

    void AdvancePhase() { phase.Advance(increment_frames, increment_fraction); }

    // Writes this block's modulation multipliers - one struct write, called
    // from the control tick (VoiceManager::TickModulation), consumed at the
    // top of Render()'s per-voice slice. Callback-safe.
    void SetBlockModulation(const ModDestinations& mods) {
        mod_cutoff_mul = mods.cutoff_mul;
        mod_gain_mul = mods.gain_mul;
        mod_pitch_mul = mods.pitch_mul;
        mod_pan_offset = mods.pan_offset;
    }
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
    // Incoming key used to release the voice. 0xFF means use `note`. This is
    // distinct in drum mode, where `note` is forced to root_note to disable
    // pitch tracking but note-off must still match the pad key that fired.
    uint8_t trigger_note = 0xFF;
    uint8_t velocity = 127;
    float pan = 0.5f;
    uint8_t root_note = 60;  // note at which `sample` plays at its recorded pitch

    // Instrument-model routing (instrument-model.md §3). slot identifies the
    // owning instrument (for StopSlot on rebind); choke_group != 0 mutes
    // other voices in the same group at trigger time (open/closed hat).
    uint8_t slot = 0;
    uint8_t choke_group = 0;
    bool one_shot = false;

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

    // Second envelope (param-locks-and-modulation.md §4), exposed only as
    // SRC_ENV_FILTER - it does not touch the filter itself except through a
    // mod slot routing it there. Defaults match the amp envelope above
    // rather than anything zone-derived: Zone has no ADSR-for-SRC_ENV_FILTER
    // fields yet (that's a wire-Zone chunk version bump, roadmap Phase 2.5
    // item 4, still open), so every trigger gets this same shape until a
    // zone can carry its own.
    float filter_env_attack_s = 0.001f;
    float filter_env_decay_s = 0.05f;
    float filter_env_sustain_level = 0.8f;
    float filter_env_release_s = 0.1f;
};

// Resolves a voice's owning instrument slot (Voice::slot) to that
// instrument's fixed 8-entry ModSlot array (param-locks-and-modulation.md
// §3/§9 stage 4). Function pointer + context, not std::function - the same
// reasoning as instrument.hpp's SampleResolver: this keeps VoiceManager
// HAL-free and independent of the instrument model, which sits a layer
// above it (instrument.hpp includes voice_manager.hpp, not the reverse).
// A null result (no resolver bound, or an out-of-range slot) means "no
// slots" - EvaluateModMatrix() already treats that as an identity no-op.
struct ModSlotResolver {
    const void* ctx = nullptr;
    const ModSlot* (*resolve)(const void* ctx, uint8_t slot) = nullptr;

    const ModSlot* Get(uint8_t slot) const { return resolve ? resolve(ctx, slot) : nullptr; }
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
        // A zeroed seed is a fixed point of xorshift (0 stays 0 forever), which
        // would make SRC_RANDOM sample the same -1.0f on every voice for the
        // life of the engine - exactly the live_pitch_scale_ bug this
        // function's own comment warns about, so it gets the same treatment.
        rng_ = 0x2545F491u;
    }

    // Pushes live parameter edits onto every SOUNDING voice, so a filter
    // sweep is audible on notes that are already playing rather than only on
    // the next one. Callback-safe: fixed iteration, no allocation, no I/O.
    //
    // Intended to be driven at block rate from the audio callback, and only
    // when something actually changed - see the caller's dirty flag. It is
    // cheap but not free: SetResonance() and SetCutoff() each recompute the
    // coefficients (including a tan()) per voice. Every voice is handed the
    // SAME cutoff and resonance here, so if this ever shows up in a DWT profile
    // the fix is to compute the coefficients once and share them, not to update
    // less often.
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
            v.base_cutoff_hz = p.filter_cutoff_hz;
            v.filter.SetCutoff(v.base_cutoff_hz * v.mod_cutoff_mul);
            if (!v.envelope.IsReleasing()) {
                v.envelope.SetParams(p.attack_s, p.decay_s, p.sustain_level, p.release_s);
            }
            // Pan is a gain pair recomputed per block in Render(), so writing
            // it here is heard on the next block without a click.
            v.pan = p.pan;
            // Multiply the note's own increment rather than overwrite it, so a
            // live transpose stacks on key tracking instead of flattening every
            // voice to the same rate.
            v.SetIncrement(v.base_increment * live_pitch_scale_);
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
        v.note = params.trigger_note == 0xFF ? params.note : params.trigger_note;
        v.slot = params.slot;
        v.choke_group = params.choke_group;
        v.one_shot = params.one_shot;
        v.age = next_age_++;

        // A stolen voice keeps its struct - without this reset it would
        // render its first block or two of the new note with the previous
        // note's leftover modulation until the next control tick overwrites
        // it. TickModulation() is expected to run before Render() each
        // callback, but Trigger() must not depend on that ordering.
        v.mod_cutoff_mul = 1.0f;
        v.mod_gain_mul = 1.0f;
        v.mod_pitch_mul = 1.0f;
        v.mod_pan_offset = 0.0f;

        // Per-trigger modulation sources (§3): sampled once, held constant
        // for the voice's life. SRC_RANDOM reuses the sample-and-hold xorshift
        // idiom lfo.hpp already established, seeded once at Init() so a host
        // test sees the same sequence every run.
        v.mod_velocity = static_cast<float>(params.velocity) / 127.0f;
        v.mod_note = static_cast<float>(params.note) / 127.0f;
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        v.mod_random = (static_cast<float>(rng_ >> 8) / 8388608.0f) - 1.0f;

        v.start_frame = params.start_frame < params.sample_frames ? params.start_frame : 0;
        v.end_frame = (params.end_frame == 0 || params.end_frame > params.sample_frames)
                          ? params.sample_frames
                          : params.end_frame;
        v.loop = params.loop;
        v.loop_start = params.loop_start < v.end_frame ? params.loop_start : v.start_frame;
        v.loop_end =
            (params.loop_end == 0 || params.loop_end > v.end_frame) ? v.end_frame : params.loop_end;
        // A degenerate loop region (loop_end <= loop_start + 1) has no playable
        // length: Render()'s wrap check would reset phase to loop_start every
        // sample, freezing the voice on one value for as long as it's held.
        // Trigger() is the single place that establishes region invariants, so
        // enforce it here rather than trusting every future caller (e.g. the
        // Phase 2.5 zone-sync path) to pre-validate.
        if (v.loop && v.loop_end <= v.loop_start + 1)
            v.loop = false;
        v.phase.SetFrame(v.start_frame);

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
        v.SetIncrement(v.base_increment * live_pitch_scale_);

        v.filter.Init(sample_rate_);
        v.filter.SetResonance(params.filter_resonance);
        v.base_cutoff_hz = params.filter_cutoff_hz;
        v.filter.SetCutoff(v.base_cutoff_hz);
        v.filter.Reset();

        v.envelope.Init(sample_rate_);
        v.envelope.SetParams(
            params.attack_s, params.decay_s, params.sustain_level, params.release_s);
        v.envelope.Retrigger();

        v.env2.Init(sample_rate_);
        v.env2.SetParams(params.filter_env_attack_s,
                         params.filter_env_decay_s,
                         params.filter_env_sustain_level,
                         params.filter_env_release_s);
        v.env2.Retrigger();
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
                !voices_[i].one_shot && !voices_[i].envelope.IsReleasing()) {
                if (found < 0 || voices_[i].age >= newest_age) {
                    found = i;
                    newest_age = voices_[i].age;
                }
            }
        }
        if (found >= 0) {
            voices_[static_cast<size_t>(found)].envelope.Release();
            voices_[static_cast<size_t>(found)].env2.Release();
        }
    }

    // Releases every held voice fired by `note` in one instrument slot,
    // except one-shot zones. Layered SFZ regions deliberately release
    // together; the legacy unscoped Release(note) above keeps its
    // newest-voice behavior for the Phase-1 single-sample fallback.
    void ReleaseSlot(uint8_t note, uint8_t slot) {
        for (auto& v: voices_) {
            if (v.state == VoiceState::Playing && v.note == note && v.slot == slot && !v.one_shot &&
                !v.envelope.IsReleasing()) {
                v.envelope.Release();
                v.env2.Release();
            }
        }
    }

    /**
     * @brief Binds the per-track mixer applied in Render(), or nullptr.
     *
     * Optional on purpose: nullptr reproduces the pre-mixer behaviour exactly,
     * which is what lets the existing voice tests keep asserting the unmixed
     * sum. The mixer is not owned and must outlive this object - on the device
     * both are engine-global statics.
     */
    void SetTrackMixer(const WaveX::Mix::TrackMixer* mixer) { track_mixer_ = mixer; }

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

            // Modulation-matrix destinations, applied once per voice per
            // block (param-locks-and-modulation.md §3) - never per sample.
            // Guarded on != 1.0 so a voice nothing modulates pays neither the
            // filter's tan() recompute nor an increment rewrite; an active
            // LFO/matrix slot drives its mod_*_mul away from identity every
            // tick, so this still runs whenever modulation is actually live.
            if (v.mod_pitch_mul != 1.0f) {
                v.SetIncrement(v.base_increment * live_pitch_scale_ * v.mod_pitch_mul);
            }
            if (v.mod_cutoff_mul != 1.0f) {
                v.filter.SetCutoff(v.base_cutoff_hz * v.mod_cutoff_mul);
            }

            // Track gain and pan fold in HERE - once per voice per block,
            // outside the sample loop below - so the mixer costs two multiplies
            // and an add per voice per block and nothing at all per sample.
            // That is the whole reason output-routing-and-mixer.md §1 puts the
            // application point at the voice's existing gain/pan rather than
            // adding a stage to the sum.
            float pan = v.pan + v.mod_pan_offset;
            float gain = v.gain * v.mod_gain_mul;
            if (track_mixer_) {
                gain *= track_mixer_->GainFor(v.slot);
                // pan_offset is -1..+1 added onto a 0..1 voice pan, per the
                // design. Clamped, so a hard offset pins rather than wrapping
                // through the opposite channel.
                pan += track_mixer_->PanOffsetFor(v.slot);
            }
            pan = pan < 0.0f ? 0.0f : (pan > 1.0f ? 1.0f : pan);
            const float left_gain = gain * (1.0f - pan);
            const float right_gain = gain * pan;
            const uint32_t last_valid_frame = v.end_frame - 1;
            const uint32_t loop_len = v.loop_end - v.loop_start;

            for (size_t i = 0; i < block_size; ++i) {
                bool holding_release_tail = false;
                if (v.loop && v.phase.Frame() >= v.loop_end) {
                    // Wrap by the loop length so the fractional phase (and
                    // with it the exact loop period/pitch) is preserved. The
                    // window is [loop_start, loop_end): its final frame does
                    // get rendered, interpolating toward loop_start below.
                    v.phase.SubtractFrames(loop_len);
                    if (v.phase.Frame() >= v.loop_end || v.phase.Frame() < v.loop_start) {
                        // Phase far outside the window (start_frame beyond
                        // loop_end, or increment > loop length): snap rather
                        // than loop an unbounded number of subtractions here.
                        v.phase.SetFrame(v.loop_start);
                    }
                } else if (!v.loop && v.phase.Frame() >= last_valid_frame) {
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
                    idx0 = last_valid_frame;
                    // frac is 0, so idx1's sample is never blended in - but the
                    // read still happens, and idx0 == end_frame - 1 means
                    // idx0 + 1 == end_frame, one frame past this voice's region
                    // (the whole allocation when end_frame == sample_frames).
                    // Mirror the non-tail branch's clamp instead of reading it.
                    idx1 = idx0;
                    frac = 0.0f;
                } else {
                    idx0 = v.phase.Frame();
                    if (v.loop && idx0 + 1 >= v.loop_end && idx0 >= v.loop_start) {
                        // Circular seam: the loop window's final frame
                        // interpolates toward loop_start, not toward the
                        // frame after the window (which may be trimmed-off
                        // audio, or out of bounds when loop_end ==
                        // sample_frames).
                        idx1 = v.loop_start;
                    } else {
                        if (idx0 >= v.sample_frames - 1)
                            idx0 = v.sample_frames -
                                   2;  // clamp: envelope release masks the tail anyway
                        idx1 = idx0 + 1;
                    }
                    frac = v.phase.Fraction();
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

                v.AdvancePhase();
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
                v.env2.SetReleaseTime(fast_release_s);
                v.env2.Release();
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

    /**
     * @brief Evaluates the modulation matrix for every sounding voice and
     * writes the result via Voice::SetBlockModulation() (§3).
     *
     * Intended to run once per control tick from the audio callback - on
     * this engine one callback IS one 1kHz tick (timebase.hpp), so this is
     * called once per block, not once per sample. `resolver` looks up each
     * voice's OWN instrument slot's matrix (Voice::slot, set at Trigger()
     * from VoiceTriggerParams::slot) - the mod matrix is instrument-scoped
     * (§3), so two voices from different slots can be modulated completely
     * differently in the same tick.
     *
     * `global` carries this tick's engine-wide sources (LFO1/2, macros,
     * modwheel/aftertouch) shared by every voice. Per-trigger sources
     * (velocity/note/random) were already sampled into the voice at
     * Trigger() and are substituted in here, not read from `global`.
     * SRC_ENV_FILTER is likewise per-voice: `block_size` advances each
     * voice's second envelope by one block (Envelope::AdvanceBlock(), §4) so
     * its current level feeds this tick's evaluation - it is a control-rate
     * source, so this is the only place it ever advances.
     *
     * Applies to every sounding voice, including one already in its release
     * tail - a filter sweep or LFO wobble that froze at note-off would sound
     * like the modulation jammed, the same reasoning ApplyLiveParams's filter
     * path already follows. (Unlike ApplyLiveParams, there is no envelope
     * write here to guard for env1: the matrix's only destinations are
     * cutoff, gain, pitch and pan. env2 releases normally through its own
     * Release()/Choke() calls same as env1, so it needs no separate guard
     * either.)
     */
    void TickModulation(const ModSlotResolver& resolver,
                        const ModSources& global,
                        uint32_t block_size) {
        for (auto& v: voices_) {
            if (v.state != VoiceState::Playing)
                continue;
            const ModSlot* slots = resolver.Get(v.slot);
            ModSources sources = global;
            sources.velocity = v.mod_velocity;
            sources.note = v.mod_note;
            sources.random = v.mod_random;
            sources.env_filter = v.env2.AdvanceBlock(block_size);
            v.SetBlockModulation(EvaluateModMatrix(slots, slots ? kMaxModSlots : 0, sources));
        }
    }

   private:
    /// Optional per-track mixer; nullptr means the pre-mixer behaviour.
    /// Not owned - see SetTrackMixer().
    const WaveX::Mix::TrackMixer* track_mixer_ = nullptr;
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
    // SRC_RANDOM sample-and-hold seed (lfo.hpp's xorshift idiom), advanced
    // once per Trigger(). Non-zero default MUST also be set in Init() -
    // see that function's comment.
    uint32_t rng_ = 0x2545F491u;
};

}  // namespace AudioEngine
}  // namespace WaveX
