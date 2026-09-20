#pragma once
#include "voice_lfo.hpp"

// Polyphonic RAM sample player with a configured mono render-channel budget.
// Each note/layer owns one or two source cursors, one envelope/modulation
// lifetime, and one mono or two independent stereo filter channels. Stereo
// sources share their L/R cursor; explicit Mono averages their PCM channels.
// Sample storage and note-to-zone resolution belong to the engine/Pool.
//
// Trigger, Release and Render are callback-safe: fixed storage, bounded
// allocation/stealing scans, no heap, blocking, I/O or logging. The existing
// portable linear interpolation remains host-testable; target callback cost
// must be measured with DWT before claiming hardware capacity. Concurrent
// streamed voices remain separate from this RAM renderer.

#include "config/hardware_config.h"
#include "memory_sections.h"
#include "spi_protocol/protocol.h"

#include "audio/mod_matrix.hpp"
#include "audio/track_mix.hpp"
#include "envelope.hpp"
#include "fade.hpp"
#include "live_note_id.hpp"
#include "note_group_admission.hpp"
#include "note_pitch_table.hpp"
#include "note_trigger_batch.hpp"
#include "profiling/callback_detail.hpp"
#include "voice_filter.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

static_assert(WAVEX_NUM_VOICES >= 1 && WAVEX_NUM_VOICES <= 64,
              "WAVEX_NUM_VOICES (hardware_config.h) sizes every per-voice array here");

enum class VoiceState : uint8_t { Idle, Playing };

// Playback position with an exact 32-bit frame index and a Q24 fractional
// component. A single float cannot advance by one frame once it reaches 2^24,
// which is well inside the duration of a mono sample that fits the SDRAM
// arena. Keeping the integer and fraction separate also makes the callback's
// per-sample advance two integer adds with bounded integer arithmetic.
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

// Bits use the stable ControlParameter ids 2..9. Playback/gain locks are
// baked into the resolved trigger and have no live replacement path.
constexpr uint16_t VoiceLockBit(uint8_t parameter) {
    return parameter < 16 ? static_cast<uint16_t>(1u << parameter) : 0;
}
struct VoiceAmpParams {
    float attack = 0.001f, decay = 0.05f, sustain = 0.8f, release = 0.1f;
};

struct VoiceSampleState {
    const int16_t* sample = nullptr;  // RAM-resident, interleaved; not owned by Voice
    uint32_t sample_frames = 0;
    uint8_t src_channels = 1;  // native PCM interleave stride
    bool stereo = false;       // preserve both source channels for this note
    PlaybackPhase phase;       // exact frame + fractional playback position
    float increment = 1.0f;    // playback rate (pitch), from note/root_note
    uint32_t increment_frames = 1;
    uint32_t increment_fraction = 0;
    // The increment this voice's own note implies, before any live transpose.
    // Kept so ApplyLiveParams can re-apply a pitch offset without losing key
    // tracking - recomputing from `increment` would compound each edit.
    float base_increment = 1.0f;
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

    void SetIncrement(float rate) {
        increment = rate;
        PlaybackPhase::SplitRate(rate, increment_frames, increment_fraction);
    }

    void AdvancePhase() { phase.Advance(increment_frames, increment_fraction); }

    float source_level = 1.0f;
    // Dry source properties survive a zero level/mix and repeated live edits.
    float dry_level = 1.0f, dry_increment = 1.0f, key_ratio = 1.0f;
    uint8_t oscillator = 0xFF;
};

// The base is the primary sample cursor; the second cursor has independent
// region, rate and lifetime. Both feed this voice's shared DSP chain.
struct Voice : VoiceSampleState {
    VoiceSampleState secondary;

    VoiceState state = VoiceState::Idle;
    float gain = 0.0f;  // 0..1, derived from velocity (× gain_mul)
    float dry_gain = 0, zone_pan = .5f;
    float lfo_pitch_ratio = 1;
    bool lfo_pitch_known = false;
    float pan = 0.5f;                  // 0=left, 1=right, linear (not equal-power)
    uint8_t note = 0;                  // MIDI note that triggered this voice
    uint16_t start_offset_frames = 0;  // consumed by the next Render() call
    uint8_t track = 0;                 // Track that owns this voice
    uint8_t choke_group = 0;           // 0 = none; 1..N = mutual-exclusion group (open/closed hat)
    bool own_filter_env = false;       // zone overrides survive Instrument live edits
    uint16_t param_lock_mask = 0;
    VoiceAmpParams amp_params;
    float base_resonance = 0;
    float locked_pitch_scale = 1;
    bool one_shot = false;  // ignore note-off; stop at the sample/region end
    uint32_t age = 0;       // trigger order, for stealing/release-newest-first
    uint64_t group_id = 0;  // one admission identity shared by every layer
    LiveNoteId live_note;   // empty for sequencer/audition triggers

    uint8_t render_channels = 1;  // reservation lasts through the release tail
    VoiceFilter filter, right_filter;
    Envelope envelope;
    // Second envelope (param-locks-and-modulation.md §4), exposed only as
    // SRC_ENV_FILTER. Unlike `envelope` above, this is a MODULATION SOURCE,
    // not audio: nothing reads it between control ticks, so it advances via
    // AdvanceBlock() once per block from TickModulation() rather than
    // Process() once per sample - the same audible output either way, at a
    // fraction of the cost.
    Envelope env2;
    Envelope env3;
    VoiceLfo lfo[Protocol::INST_LFO_COUNT];
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
    ModScaleCache mod_scale_cache;
    float mod_cutoff_mul = 1.0f;
    bool modulation_cutoff_dirty = false;
    float mod_gain_mul = 1.0f;
    float mod_pitch_mul = 1.0f;
    float mod_oscillator_pitch_mul[2] = {1.0f, 1.0f};
    float mod_pan_offset = 0.0f;
    float mod_resonance_offset = 0.0f;

    // Per-trigger modulation sources (SRC_VELOCITY/SRC_NOTE/SRC_RANDOM),
    // sampled once at Trigger() and held constant for the voice's lifetime -
    // §3 requires these NOT be re-sampled every control tick.
    float mod_velocity = 0.0f;
    float mod_note = 0.0f;
    float mod_random = 0.0f;

    bool IsFree() const { return state == VoiceState::Idle; }

    // Writes this block's modulation multipliers - one struct write, called
    // from the control tick (VoiceManager::TickModulation), consumed at the
    // top of Render()'s per-voice slice. Callback-safe.
    void SetBlockModulation(const ModDestinations& mods) {
        modulation_cutoff_dirty = modulation_cutoff_dirty || mod_cutoff_mul != mods.cutoff_mul ||
                                  mod_resonance_offset != mods.resonance_offset;
        mod_cutoff_mul = mods.cutoff_mul;
        mod_gain_mul = mods.gain_mul;
        mod_pitch_mul = mods.pitch_mul;
        mod_oscillator_pitch_mul[0] = mods.oscillator_pitch_mul[0];
        mod_oscillator_pitch_mul[1] = mods.oscillator_pitch_mul[1];
        mod_pan_offset = mods.pan_offset;
        mod_resonance_offset = mods.resonance_offset;
    }

    float SourcePitchModulation(const VoiceSampleState& source) const {
        // A lone Osc 2 occupies the primary cursor; cursor position is not
        // oscillator identity. Unassigned/sample-preview sources stay common-only.
        return mod_pitch_mul *
               (source.oscillator < 2 ? mod_oscillator_pitch_mul[source.oscillator] : 1.f);
    }
};

// Named-argument trigger parameters (roadmap-item-4-sized Trigger() calls
// don't fit sanely as positional arguments - see the wire-struct-hygiene
// precedent elsewhere in this codebase for why named fields over positional
// sprawl). Only `sample`/`sample_frames` are required; everything else has
// a sensible default for a plain one-shot voice.
// Primary-source fields remain directly addressable for single-sample callers.
// A second source carries no duplicate filter, envelope or Track authority.
struct VoiceSampleParams {
    const int16_t* sample = nullptr;
    uint32_t sample_frames = 0, sample_rate_hz = 0;
    uint8_t channels = 1, note = 60, root_note = 60;
    float pitch_ratio_mul = 1.0f, source_level = 1.0f;
    float dry_pitch_ratio = 1.0f, dry_level = 1.0f;
    uint8_t oscillator = 0xFF, key_note = 60;
    bool keytrack = true, drum = false;
    bool mono = false;  // force (L+R)/2; false preserves native stereo
    uint32_t start_frame = 0, end_frame = 0;
    bool loop = false;
    uint32_t loop_start = 0, loop_end = 0;
    uint16_t fade_in_ms = 0, fade_out_ms = 0;
};
struct VoiceTriggerParams : VoiceSampleParams {
    LiveNoteId live_note;
    VoiceSampleParams secondary;
    uint8_t trigger_note = 0xFF, velocity = 127;
    uint16_t start_offset_frames = 0;
    float pan = 0.5f;
    uint8_t track = 0, choke_group = 0;
    bool own_filter_env = false;
    uint16_t param_lock_mask = 0;
    float locked_pitch_scale = 1;
    bool one_shot = false;
    float gain_mul = 1.0f, instrument_gain = 1.0f, zone_pan = .5f;
    float filter_cutoff_hz = 20000.0f, filter_resonance = 0.0f;
    SvfFilter::Mode filter_mode = SvfFilter::Mode::LowPass;
    FilterTopology filter_topology = FilterTopology::WaveXSvf;
    FilterConfig filter_config;  // slope and drive, Instrument-owned like the mode
    float attack_s = 0.001f, decay_s = 0.05f, sustain_level = 0.8f, release_s = 0.1f;
    float filter_env_attack_s = 0.001f, filter_env_decay_s = 0.05f;
    float filter_env_sustain_level = 0.8f, filter_env_release_s = 0.1f;
    float aux_env_attack_s = 0.001f, aux_env_decay_s = 0.05f;
    float aux_env_sustain_level = 0.8f, aux_env_release_s = 0.1f;
    Protocol::InstLfoSettings lfo[Protocol::INST_LFO_COUNT];
};

// Resolves a voice's owning Track (Voice::track) to that
// instrument's fixed 8-entry ModSlot array (param-locks-and-modulation.md
// §3/§9 stage 4). Function pointer + context, not std::function - the same
// reasoning as instrument.hpp's SampleResolver: this keeps VoiceManager
// HAL-free and independent of the instrument model, which sits a layer
// above it (instrument.hpp includes voice_manager.hpp, not the reverse).
// A null result (no resolver bound, or an out-of-range track) means "no
// slots" - EvaluateModMatrix() already treats that as an identity no-op.
struct ModSlotResolver {
    const void* ctx = nullptr;
    const ModSlot* (*resolve)(const void* ctx, uint8_t track) = nullptr;

    const ModSlot* Get(uint8_t track) const { return resolve ? resolve(ctx, track) : nullptr; }
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
struct VoiceInstrumentParams {
    struct Oscillator {
        float level = 1, tune_ratio = 1;
        bool keytrack = true;
    };
    bool enabled = false;
    SvfFilter::Mode filter_mode = SvfFilter::Mode::LowPass;
    FilterTopology filter_topology = FilterTopology::WaveXSvf;
    FilterConfig filter_config;
    float gain = 1, pan = .5f;
    Oscillator osc[2];
    VoiceAmpParams env[2];
    Protocol::InstLfoSettings lfo[Protocol::INST_LFO_COUNT];
};
struct VoiceLiveParams {
    // Which Track these values describe. A knob edits ONE Track's Instrument
    // (track-and-patch-model.md §3.2), so pushing the result onto every
    // sounding voice would let Track 3's cutoff move Track 5's held notes.
    // 0xFF means "every Track", which is what Init() publishes.
    uint8_t track = 0xFF;
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
    VoiceInstrumentParams instrument;
};

// A value, not a second cursor: both channels advance at exactly the same rate.
struct StereoFrame {
    float left = 0, right = 0;
    StereoFrame& operator*=(float gain) {
        left *= gain;
        right *= gain;
        return *this;
    }
    StereoFrame operator*(float gain) const { return {left * gain, right * gain}; }
    StereoFrame operator+(const StereoFrame& other) const {
        return {left + other.left, right + other.right};
    }
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
        frame_clock_ = beat_clock_ = 0;
        tempo_bpm_ = 0;
        next_group_id_ = 1;
        next_age_ = 0;
        track_bindings_.fill(1);
        note_pitch_.Init();
        SetTempo(120);
        live_pitch_scales_.fill(1.0f);
        // DSP initialization belongs to startup, not every note-on. Trigger
        // only installs tuning and resets the active filter's integrators.
        for (auto& voice: voices_) {
            voice = Voice{};
            voice.filter.Init(sample_rate_);
            voice.right_filter.Init(sample_rate_);
            voice.envelope.Init(sample_rate_);
            voice.env2.Init(sample_rate_);
            voice.env3.Init(sample_rate_);
        }
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
    // cheap but not free: one combined cutoff/resonance update recomputes
    // the coefficients per voice. The effective cutoff is voice-owned:
    // pad overrides and modulation can make it differ within one Track.
    //
    // Envelope rates are deliberately NOT written to a voice that is already
    // releasing. Choke() forces a short release onto a voice immediately
    // before Release() (open/closed hat), and rewriting the ADSR here would
    // hand that voice its full-length release back mid-choke - the hat would
    // not cut off. Filter changes still apply to releasing voices, because a
    // sweep should stay audible through the release tail.
    void SetTempo(float bpm) {
        if (bpm == tempo_bpm_)
            return;
        tempo_bpm_ = bpm;
        beat_step_ = VoiceLfo::BeatStep(bpm, sample_rate_);
    }

    WAVEX_ITCM_CODE_NAMED("voice.ApplyLiveParams") void ApplyLiveParams(const VoiceLiveParams& p) {
        // One pow() per call, not one per voice: this runs at block rate from
        // the audio callback whenever a control moved, and eight of them would
        // be eight transcendentals inside the deadline for no benefit.
        const float live_pitch_scale = std::pow(2.0f, p.pitch_semitones / 12.0f);
        if (p.track == 0xFF) {
            live_pitch_scales_.fill(live_pitch_scale);
        } else if (p.track < live_pitch_scales_.size()) {
            live_pitch_scales_[p.track] = live_pitch_scale;
        }
        for (auto& v: voices_) {
            if (v.state != VoiceState::Playing)
                continue;
            if (p.track != 0xFF && v.track != p.track)
                continue;
            const auto unlocked = [&v](uint8_t id) {
                return !(v.param_lock_mask & VoiceLockBit(id));
            };
            if (unlocked(Protocol::PARAM_FILTER_RESONANCE))
                v.base_resonance = p.filter_resonance;
            if (!v.own_filter_env && unlocked(Protocol::PARAM_FILTER_CUTOFF))
                v.base_cutoff_hz = p.filter_cutoff_hz;
            v.filter.SetParameters(v.base_cutoff_hz * v.mod_cutoff_mul,
                                   v.base_resonance + v.mod_resonance_offset);
            if (v.render_channels == 2)
                v.right_filter.SetParameters(v.base_cutoff_hz * v.mod_cutoff_mul,
                                             v.base_resonance + v.mod_resonance_offset);
            if (!v.own_filter_env && !v.envelope.IsReleasing()) {
                auto& amp = v.amp_params;
                if (unlocked(Protocol::PARAM_ENVELOPE_ATTACK))
                    amp.attack = p.attack_s;
                if (unlocked(Protocol::PARAM_ENVELOPE_DECAY))
                    amp.decay = p.decay_s;
                if (unlocked(Protocol::PARAM_ENVELOPE_SUSTAIN))
                    amp.sustain = p.sustain_level;
                if (unlocked(Protocol::PARAM_ENVELOPE_RELEASE))
                    amp.release = p.release_s;
                v.envelope.SetParams(amp.attack, amp.decay, amp.sustain, amp.release);
            }
            if (p.instrument.enabled && v.oscillator < 2) {
                // SetConfig is a no-op unless slope or drive changed, so an
                // unrelated live edit neither retunes nor resets the filter.
                v.filter.SetConfig(p.instrument.filter_config);
                if (v.render_channels == 2)
                    v.right_filter.SetConfig(p.instrument.filter_config);
                v.filter.SetTopology(p.instrument.filter_topology);
                if (v.render_channels == 2)
                    v.right_filter.SetTopology(p.instrument.filter_topology);
                v.filter.SetMode(p.instrument.filter_mode);
                if (v.render_channels == 2)
                    v.right_filter.SetMode(p.instrument.filter_mode);
                v.gain = v.dry_gain * p.instrument.gain;
                ApplySourceLive(v, p.instrument);
                if (v.secondary.sample)
                    ApplySourceLive(v.secondary, p.instrument);
                const auto& e2 = p.instrument.env[0];
                const auto& e3 = p.instrument.env[1];
                if (!v.env2.IsReleasing())
                    v.env2.SetParams(e2.attack, e2.decay, e2.sustain, e2.release);
                if (!v.env3.IsReleasing())
                    v.env3.SetParams(e3.attack, e3.decay, e3.sustain, e3.release);
                const bool follow =
                    (p.instrument.lfo[0].pitch_follow && !p.instrument.lfo[0].sync_div) ||
                    (p.instrument.lfo[1].pitch_follow && !p.instrument.lfo[1].sync_div);
                if (follow && !v.lfo_pitch_known) {
                    v.lfo_pitch_ratio = note_pitch_.Ratio(v.note, 60);
                    v.lfo_pitch_known = true;
                }
                for (uint8_t i = 0; i < Protocol::INST_LFO_COUNT; ++i)
                    v.lfo[i].UpdateSettings(p.instrument.lfo[i], sample_rate_, v.lfo_pitch_ratio);
            }
            if (unlocked(Protocol::PARAM_PAN))
                v.pan = p.instrument.enabled && v.oscillator < 2
                            ? std::clamp(v.zone_pan + p.instrument.pan + p.pan - 1.f, 0.f, 1.f)
                            : p.pan;
            // Multiply the note's own increment rather than overwrite it, so a
            // live transpose stacks on key tracking instead of flattening every
            // voice to the same rate.
            v.SetIncrement(v.base_increment * VoicePitchScale(v));
            if (v.secondary.sample)
                v.secondary.SetIncrement(v.secondary.base_increment * VoicePitchScale(v));
        }
    }

    void Trigger(const VoiceTriggerParams& params) { TriggerGroup(&params, 1); }

    // One callback-owned transaction per musical note. Plan the total stereo/
    // layer cost first; refusal cannot steal or choke an existing note. The
    // returned identity survives render-slot reuse and can scope a future
    // sequencer gate release without cutting off a later note of the same pitch.
    WAVEX_ITCM_CODE_NAMED("voice.TriggerGroup")
    uint64_t TriggerGroup(const VoiceTriggerParams* layers,
                          uint8_t count,
                          Allocation::Policy policy = {}) {
        if (!layers || !count || count > voices_.size() || !next_group_id_ ||
            layers[0].track >= track_bindings_.size())
            return 0;
        const uint8_t track = layers[0].track;
        const auto note = [](const VoiceTriggerParams& p) {
            return p.trigger_note == 0xFF ? p.note : p.trigger_note;
        };
        uint8_t channels = 0;
        bool has_choke = false;
        for (uint8_t i = 0; i < count; ++i) {
            const auto& p = layers[i];
            if (!p.sample || p.sample_frames < 2 || p.track != track ||
                note(p) != note(layers[0]) ||
                p.start_offset_frames != layers[0].start_offset_frames)
                return 0;
            channels += TriggerChannels(p);
            has_choke = has_choke || p.choke_group != 0;
        }
        typename Allocation::Admission<>::Snapshot groups{};
        for (size_t i = 0; i < voices_.size(); ++i) {
            const auto& v = voices_[i];
            if (v.IsFree())
                continue;
            size_t group = 0;
            while (group < i && groups[group].id != v.group_id)
                ++group;
            auto& g = groups[group];
            if (!g.slots) {
                g.id = v.group_id;
                g.owner = {track_bindings_[v.track], v.track};
                g.releasing = true;
            }
            g.slots |= uint64_t{1} << i;
            g.channels += v.render_channels;
            bool releasing = v.envelope.IsReleasing();
            // Preserve choke-before-steal priority without mutating envelopes
            // until admission succeeds. A partly choked group remains held.
            if (has_choke && v.track == track && v.choke_group)
                for (uint8_t layer = 0; layer < count; ++layer)
                    releasing = releasing || layers[layer].choke_group == v.choke_group;
            g.releasing = g.releasing && releasing;
        }
        const auto plan = Allocation::Admission<>::Build(
            groups, {{track_bindings_[track], track}, policy, count, channels});
        if (plan.result != Allocation::Result::Accepted)
            return 0;
        for (size_t i = 0; i < voices_.size(); ++i)
            if (plan.retire_slots & (uint64_t{1} << i))
                voices_[i].state = VoiceState::Idle;
        // Choke only pre-existing voices, never siblings of the incoming note.
        for (uint8_t i = 0; i < count; ++i)
            Choke(layers[i].choke_group, .005f, track);
        const uint64_t id = next_group_id_++;
        uint8_t layer = 0;
        for (size_t i = 0; i < voices_.size(); ++i) {
            if (!(plan.new_slots & (uint64_t{1} << i)))
                continue;
            StartVoice(voices_[i], layers[layer++], next_age_++, NextTriggerRandom());
            voices_[i].group_id = id;
        }
        return id;
    }

    // Same-frame notes only. Describe supplies validated costs/chokes from an
    // immutable prepared map; materialize supplies the corresponding parameters
    // for a surviving (request, layer). Neither callback may mutate this manager.
    // Only eight compact slot records and one full trigger live on the stack.
    struct IgnoreAdmission {
        void operator()(uint16_t, uint64_t) const {}
    };
    template <typename Describe, typename Materialize, typename Admitted = IgnoreAdmission>
    WAVEX_ITCM_CODE_NAMED("voice.TriggerBatch")
    uint64_t TriggerBatch(uint16_t count,
                          Describe describe,
                          Materialize materialize,
                          Admitted admitted = {}) {
        if (count > 32)
            return 0;
        NoteTriggerBatch batch;
        uint32_t ages[WAVEX_NUM_VOICES]{}, random[WAVEX_NUM_VOICES]{};
        for (size_t i = 0; i < voices_.size(); ++i) {
            const auto& v = voices_[i];
            if (!v.IsFree())
                batch.slots[i] = {v.group_id,
                                  {track_bindings_[v.track], v.track},
                                  NoteTriggerBatch::kExisting,
                                  0,
                                  v.render_channels,
                                  v.choke_group,
                                  v.envelope.IsReleasing()};
        }
        uint64_t last_id = 0;
        for (uint16_t request = 0; request < count; ++request) {
            const auto note = describe(request);
            if (note.track >= track_bindings_.size())
                continue;
            CALLBACK_DETAIL_SCOPE(SeqTrigger);
            const auto plan =
                batch.Admit(note, track_bindings_[note.track], next_group_id_, request);
            if (plan.result != Allocation::Result::Accepted)
                continue;
            last_id = next_group_id_++;
            admitted(request, last_id);
            for (size_t i = 0; i < voices_.size(); ++i)
                if (plan.new_slots & (uint64_t{1} << i)) {
                    ages[i] = next_age_++;
                    random[i] = NextTriggerRandom();
                }
        }
        for (size_t i = 0; i < voices_.size(); ++i) {
            auto& voice = voices_[i];
            const auto& slot = batch.slots[i];
            if (!slot.id) {
                voice.state = VoiceState::Idle;
                continue;
            }
            if (slot.request != NoteTriggerBatch::kExisting) {
                VoiceTriggerParams params;
                materialize(slot.request, slot.layer, params);
                CALLBACK_DETAIL_SCOPE(SeqTrigger);
                StartVoice(voice, params, ages[i], random[i]);
                voice.group_id = slot.id;
            }
            if (slot.releasing && !voice.envelope.IsReleasing())
                ChokeVoice(voice, .005f);
        }
        return last_id;
    }

    // Stale IDs are harmless after stealing, sample retirement or slot reuse.
    // As with keyboard note-off, a one-shot layer ignores a normal gate release.
    void ReleaseGroup(uint64_t id) {
        if (!id)
            return;
        for (auto& v: voices_) {
            if (!v.IsFree() && v.group_id == id && !v.one_shot && !v.envelope.IsReleasing()) {
                v.envelope.Release();
                v.env2.Release();
                v.env3.Release();
            }
        }
    }

   private:
    WAVEX_ITCM_CODE_NAMED("voice.StartVoice")
    void StartVoice(Voice& v, const VoiceTriggerParams& params, uint32_t age, uint32_t random) {
        CALLBACK_DETAIL_SCOPE(VoiceStart);
        v.state = VoiceState::Playing;
        v.render_channels = TriggerChannels(params);
        v.dry_gain = (static_cast<float>(params.velocity) / 127.0f) * params.gain_mul;
        v.gain = v.dry_gain * params.instrument_gain;
        v.zone_pan = params.zone_pan;
        v.pan = params.pan < 0.0f ? 0.0f : (params.pan > 1.0f ? 1.0f : params.pan);
        v.note = params.trigger_note == 0xFF ? params.note : params.trigger_note;
        v.start_offset_frames = params.start_offset_frames;
        v.track = params.track;
        v.live_note = params.live_note;
        v.choke_group = params.choke_group;
        v.one_shot = params.one_shot;
        v.own_filter_env = params.own_filter_env;
        v.param_lock_mask = params.param_lock_mask;
        v.locked_pitch_scale = params.locked_pitch_scale;
        v.base_resonance = params.filter_resonance;
        v.amp_params = {params.attack_s, params.decay_s, params.sustain_level, params.release_s};
        v.age = age;

        // A stolen voice keeps its struct - without this reset it would
        // render its first block or two of the new note with the previous
        // note's leftover modulation until the next control tick overwrites
        // it. TickModulation() is expected to run before Render() each
        // callback, but Trigger() must not depend on that ordering.
        v.mod_scale_cache = ModScaleCache{};
        v.mod_cutoff_mul = 1.0f;
        v.modulation_cutoff_dirty = false;
        v.mod_gain_mul = 1.0f;
        v.mod_pitch_mul = 1.0f;
        v.mod_oscillator_pitch_mul[0] = v.mod_oscillator_pitch_mul[1] = 1.f;
        v.mod_pan_offset = 0.0f;
        v.mod_resonance_offset = 0.0f;

        // Per-trigger modulation sources (§3): sampled once, held constant
        // for the voice's life. SRC_RANDOM reuses the sample-and-hold xorshift
        // idiom lfo.hpp already established, seeded once at Init() so a host
        // test sees the same sequence every run.
        v.mod_velocity = static_cast<float>(params.velocity) / 127.0f;
        v.mod_note = static_cast<float>(params.note) / 127.0f;
        v.mod_random = (static_cast<float>(random >> 8) / 8388608.0f) - 1.0f;

        {
            CALLBACK_DETAIL_SCOPE(SourceInit);
            InitSource(v, params, VoicePitchScale(v));
            v.secondary = VoiceSampleState{};
            if (params.secondary.sample && params.secondary.sample_frames >= 2)
                InitSource(v.secondary, params.secondary, VoicePitchScale(v));
        }
        {
            CALLBACK_DETAIL_SCOPE(FilterInit);
            v.filter.SetConfig(params.filter_config);

            if (v.render_channels == 2)

                v.right_filter.SetConfig(params.filter_config);
            v.filter.SetTopology(params.filter_topology);
            if (v.render_channels == 2)
                v.right_filter.SetTopology(params.filter_topology);
            v.filter.SetMode(params.filter_mode);
            if (v.render_channels == 2)
                v.right_filter.SetMode(params.filter_mode);
            v.base_cutoff_hz = params.filter_cutoff_hz;
            v.filter.SetParameters(v.base_cutoff_hz, params.filter_resonance);
            if (v.render_channels == 2)
                v.right_filter.SetParameters(v.base_cutoff_hz, params.filter_resonance);
            v.filter.Reset();
            if (v.render_channels == 2)
                v.right_filter.Reset();
        }
        {
            CALLBACK_DETAIL_SCOPE(EnvelopeInit);
            v.envelope.SetParams(
                params.attack_s, params.decay_s, params.sustain_level, params.release_s);
            v.envelope.Retrigger();

            v.env2.SetParams(params.filter_env_attack_s,
                             params.filter_env_decay_s,
                             params.filter_env_sustain_level,
                             params.filter_env_release_s);
            v.env2.Retrigger();
            v.env3.SetParams(params.aux_env_attack_s,
                             params.aux_env_decay_s,
                             params.aux_env_sustain_level,
                             params.aux_env_release_s);
            v.env3.Retrigger();
        }
        {
            CALLBACK_DETAIL_SCOPE(LfoInit);
            const bool follow = (params.lfo[0].pitch_follow && !params.lfo[0].sync_div) ||
                                (params.lfo[1].pitch_follow && !params.lfo[1].sync_div);
            v.lfo_pitch_known = follow;
            v.lfo_pitch_ratio = follow ? note_pitch_.Ratio(v.note, 60) : 1;
            for (uint8_t i = 0; i < Protocol::INST_LFO_COUNT; ++i)
                v.lfo[i].Start(params.lfo[i],
                               sample_rate_,
                               v.lfo_pitch_ratio,
                               frame_clock_,
                               beat_clock_,
                               beat_step_,
                               v.start_offset_frames,
                               random ^ (0x9e3779b9u * (i + 1u)),
                               i);
        }
    }

    uint32_t NextTriggerRandom() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    static void ChokeVoice(Voice& v, float seconds) {
        v.envelope.SetReleaseTime(seconds);
        v.envelope.Release();
        v.env2.SetReleaseTime(seconds);
        v.env3.SetReleaseTime(seconds);
        v.env2.Release();
        v.env3.Release();
    }

   public:
    void ReleaseLive(LiveNoteId id, bool through = false) {
        for (auto& v: voices_)
            if (v.state == VoiceState::Playing && !v.one_shot && v.live_note.Matches(id, through)) {
                v.envelope.Release();
                v.env2.Release();
                v.env3.Release();
            }
    }
    template <typename Released>
    void ReleaseOverflow(Released released) {
        for (auto& v: voices_)
            if (v.state == VoiceState::Playing && !v.one_shot && released(v.live_note)) {
                v.envelope.Release();
                v.env2.Release();
                v.env3.Release();
            }
    }

    // Starts the release phase of the most recently triggered still-active
    // voice for `note` (envelope decays over its release time - the voice
    // stays allocated/rendering until the envelope reaches silence, it does
    // not stop immediately). No-op if no voice is active for that note.
    void Release(uint8_t note) {
        int found = -1;
        uint32_t newest_age = 0;
        for (uint8_t i = 0; i < WAVEX_NUM_VOICES; ++i) {
            if (voices_[i].state == VoiceState::Playing && voices_[i].note == note &&
                !voices_[i].one_shot && !voices_[i].envelope.IsReleasing()) {
                if (found < 0 || voices_[i].age >= newest_age) {
                    found = i;
                    newest_age = voices_[i].age;
                }
            }
        }
        if (found >= 0) {
            ReleaseGroup(voices_[static_cast<size_t>(found)].group_id);
        }
    }

    // Releases every held voice fired by `note` on one Track,
    // except one-shot zones. Layered SFZ regions deliberately release
    // together; the legacy unscoped Release(note) above keeps its
    // newest-voice behavior for the Phase-1 single-sample fallback.
    void ReleaseTrack(uint8_t note, uint8_t track) {
        for (auto& v: voices_) {
            if (v.state == VoiceState::Playing && v.note == note && v.track == track &&
                !v.one_shot && !v.envelope.IsReleasing()) {
                v.envelope.Release();
                v.env2.Release();
                v.env3.Release();
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
    WAVEX_ITCM_CODE_NAMED("voice.Render")
    // Optional caller-owned peak hold, one cell per Track. Records the maximum
    // individual voice contribution, not a phase-dependent sum/clip meter.
    void Render(float* out_l, float* out_r, size_t block_size, float* track_peaks = nullptr) {
        for (size_t i = 0; i < block_size; ++i) {
            out_l[i] = 0.0f;
            out_r[i] = 0.0f;
        }
        for (uint8_t vi = 0; vi < WAVEX_NUM_VOICES; ++vi) {
            Voice& v = voices_[vi];
            if (v.state != VoiceState::Playing)
                continue;

            // A sequencer event may land anywhere in this audio block. Keep
            // the voice silent until that exact output frame, then clear the
            // one-shot delay so later blocks render normally. The scheduler
            // guarantees offsets below the block size; clamping here still
            // makes a future caller's larger value safe and deterministic.
            const size_t start_offset =
                v.start_offset_frames < block_size ? v.start_offset_frames : block_size;
            v.start_offset_frames = 0;

            // Modulation-matrix destinations, applied once per voice per
            // block (param-locks-and-modulation.md §3) - never per sample.
            // Guarded on != 1.0 so a voice nothing modulates pays neither the
            // filter's tan() recompute nor an increment rewrite; an active
            // LFO/matrix slot drives its mod_*_mul away from identity every
            // tick, so this still runs whenever modulation is actually live.
            const float increment =
                v.base_increment * VoicePitchScale(v) * v.SourcePitchModulation(v);
            if (v.increment != increment) {
                v.SetIncrement(increment);
            }
            // Returning modulation to identity is itself an update. Skipping
            // that transition would leave the last modulated cutoff latched.
            if (v.mod_cutoff_mul != 1.0f || v.mod_resonance_offset != 0.f ||
                v.modulation_cutoff_dirty) {
                v.filter.SetParameters(v.base_cutoff_hz * v.mod_cutoff_mul,
                                       v.base_resonance + v.mod_resonance_offset);
                if (v.render_channels == 2)
                    v.right_filter.SetParameters(v.base_cutoff_hz * v.mod_cutoff_mul,
                                                 v.base_resonance + v.mod_resonance_offset);
                v.modulation_cutoff_dirty = false;
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
                gain *= track_mixer_->GainFor(v.track);
                // pan_offset is -1..+1 added onto a 0..1 voice pan, per the
                // design. Clamped, so a hard offset pins rather than wrapping
                // through the opposite channel.
                pan += track_mixer_->PanOffsetFor(v.track);
            }
            pan = pan < 0.0f ? 0.0f : (pan > 1.0f ? 1.0f : pan);
            const bool stereo = v.render_channels == 2;
            // Mono retains linear pan; stereo uses balance (unity at center).
            const float left_gain = gain * (stereo ? std::min(1.f, 2.f * (1.f - pan)) : 1.f - pan);
            const float right_gain = gain * (stereo ? std::min(1.f, 2.f * pan) : pan);
            const bool dual = v.secondary.sample != nullptr;
            if (dual) {
                const float rate = v.secondary.base_increment * VoicePitchScale(v) *
                                   v.SourcePitchModulation(v.secondary);
                if (rate != v.secondary.increment)
                    v.secondary.SetIncrement(rate);
            }

            // Region fades, prepared once per voice per block: Prepare() holds
            // the only divides, so the per-sample Gain() below is a multiply
            // and a table lerp (fade.hpp). Inactive - and skipped - for the
            // common voice with no fades set.
            const RegionFade region_fade =
                (v.fade_in_frames != 0 || v.fade_out_frames != 0)
                    ? RegionFade::Prepare(
                          v.start_frame, v.end_frame, v.fade_in_frames, v.fade_out_frames)
                    : RegionFade{};
            const bool apply_region_fade = region_fade.Active();
            const RegionFade fade2 =
                dual && (v.secondary.fade_in_frames || v.secondary.fade_out_frames)
                    ? RegionFade::Prepare(v.secondary.start_frame,
                                          v.secondary.end_frame,
                                          v.secondary.fade_in_frames,
                                          v.secondary.fade_out_frames)
                    : RegionFade{};

            float* peak =
                track_peaks && v.track < WaveX::Mix::kNumTracks ? &track_peaks[v.track] : nullptr;
            for (size_t i = 0; i < block_size; ++i) {
                if (i < start_offset)
                    continue;
                uint32_t frame = 0;
                bool ended = false;
                StereoFrame s = ReadSource(v, frame, ended);
                if (stereo && !v.stereo)
                    s *= .5f;  // mono source centered inside a stereo submix
                if (dual) {
                    uint32_t frame2 = 0;
                    bool ended2 = false;
                    StereoFrame s2 = ReadSource(v.secondary, frame2, ended2);
                    if (stereo && !v.secondary.stereo)
                        s2 *= .5f;
                    // A shorter source falls silent while its partner continues.
                    // Only both source ends release the shared amplitude envelope.
                    if (ended && !ended2)
                        s = {};
                    if (ended2 && !ended)
                        s2 = {};
                    if (ended && ended2)
                        v.envelope.Release();
                    if (apply_region_fade)
                        s *= region_fade.Gain(frame);
                    if (fade2.Active())
                        s2 *= fade2.Gain(frame2);
                    s = s * v.source_level + s2 * v.secondary.source_level;
                    if (!ended2)
                        v.secondary.AdvancePhase();
                } else {
                    if (ended)
                        v.envelope.Release();
                    s *= v.source_level;
                    // The region fade shapes the SOURCE, before the filter,
                    // exactly as the dual path applies it. It used to sit
                    // after the filter and envelope, where a sample's
                    // fade-out multiplied the whole voice by zero past the
                    // end frame - so a resonant filter's ring through the
                    // release, and the release itself, were silenced
                    // (found 2026-09-14 chasing a ladder that would not sing).
                    if (apply_region_fade)
                        s *= region_fade.Gain(frame);
                }

                const float left = v.filter.Process(s.left);
                const float right = stereo ? v.right_filter.Process(s.right) : left;
                const float env = v.envelope.Process();
                const float contribution_l = left * env * left_gain;
                const float contribution_r = right * env * right_gain;
                out_l[i] += contribution_l;
                out_r[i] += contribution_r;
                if (peak) {
                    *peak = std::max(
                        *peak, std::max(std::fabs(contribution_l), std::fabs(contribution_r)));
                }

                if (v.envelope.IsIdle()) {
                    v.state = VoiceState::Idle;
                    break;
                }

                if (!ended)
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
        for (auto& binding: track_bindings_)
            if (++binding == 0)
                binding = 1;
    }

    // Choke group (instrument-model.md §3): forces every playing voice in
    // `group` that is not already releasing into a fast release, so the
    // classic open-hat/closed-hat mutual exclusion cuts the open hat off
    // near-instantly but click-free. `group` 0 is "no group" and never
    // chokes anything. Called from Trigger() before allocating the new
    // voice, scoped to its Track. An explicit unscoped Choke keeps the global
    // utility behavior; callback-safe (no alloc/IO). fast_release_s default 5 ms.
    void Choke(uint8_t group, float fast_release_s = 0.005f, uint8_t track = 0xFF) {
        if (group == 0)
            return;
        for (auto& v: voices_) {
            if (v.state == VoiceState::Playing && v.choke_group == group &&
                (track == 0xFF || v.track == track) && !v.envelope.IsReleasing()) {
                ChokeVoice(v, fast_release_s);
            }
        }
    }

    // Hard-stops (no release tail) only voices owned by `track`. Used when a
    // Track is rebound to a new Instrument whose sample memory is about to
    // be released - scoped so rebinding Track 3 doesn't cut off
    // slots 0-2 (instrument-model.md §4). Callback-safe.
    void StopTrack(uint8_t track) {
        for (auto& v: voices_) {
            if (v.track == track)
                v.state = VoiceState::Idle;
        }
        if (track < track_bindings_.size() && ++track_bindings_[track] == 0)
            track_bindings_[track] = 1;
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

    uint8_t ActiveChannelCount() const {
        uint8_t count = 0;
        for (const auto& v: voices_)
            if (!v.IsFree())
                count += v.render_channels;
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
     * voice's OWN Track's Instrument matrix (Voice::track, set at Trigger()
     * from VoiceTriggerParams::track) - the mod matrix is instrument-scoped
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
    WAVEX_ITCM_CODE_NAMED("voice.TickModulation")
    void TickModulation(const ModSlotResolver& resolver,
                        const ModSources& global,
                        uint32_t block_size) {
        for (auto& v: voices_) {
            if (v.state != VoiceState::Playing)
                continue;
            const ModSlot* slots = resolver.Get(v.track);
            ModSources sources = global;
            sources.velocity = v.mod_velocity;
            sources.note = v.mod_note;
            sources.random = v.mod_random;
            const uint32_t active_frames =
                block_size > v.start_offset_frames ? block_size - v.start_offset_frames : 0;
            sources.env_amp = v.envelope.Level();
            sources.env_filter = v.env2.AdvanceBlock(active_frames);
            sources.env_aux = v.env3.AdvanceBlock(active_frames);
            sources.lfo_voice = v.lfo[0].Advance(active_frames, beat_step_);
            sources.lfo_voice2 = v.lfo[1].Advance(active_frames, beat_step_);
            v.SetBlockModulation(
                EvaluateModMatrix(slots, slots ? kMaxModSlots : 0, sources, &v.mod_scale_cache));
        }
        frame_clock_ += block_size;
        beat_clock_ += uint64_t{beat_step_} * block_size;
    }

   private:
    static void ApplySourceLive(VoiceSampleState& source, const VoiceInstrumentParams& p) {
        if (source.oscillator >= 2)
            return;
        const auto& osc = p.osc[source.oscillator];
        source.source_level = source.dry_level * osc.level;
        source.base_increment =
            source.dry_increment * osc.tune_ratio * (osc.keytrack ? source.key_ratio : 1.f);
    }
    WAVEX_ITCM_CODE_NAMED("voice.InitSource")
    void InitSource(VoiceSampleState& v, const VoiceSampleParams& params, float pitch_scale) {
        v.sample = params.sample;
        v.sample_frames = params.sample_frames;
        v.src_channels = (params.channels == 2) ? 2 : 1;
        v.stereo = SourceChannels(params) == 2;
        v.source_level = params.source_level;
        v.dry_level = params.dry_level;
        v.oscillator = params.oscillator;
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
        const uint8_t key = params.oscillator < 2 ? params.key_note : params.note;
        v.key_ratio = note_pitch_.Ratio(key, params.root_note);
        v.dry_increment = rate_ratio * params.dry_pitch_ratio;
        v.base_increment = rate_ratio * params.pitch_ratio_mul *
                           (params.oscillator < 2 && !params.keytrack ? 1.f : v.key_ratio);
        v.SetIncrement(v.base_increment * pitch_scale);
    }
    // This runs once per sample per oscillator. Keep cursor state and the
    // returned frame/end flags in the caller's registers instead of spilling
    // them through an out-of-line call at audio rate.
    [[gnu::always_inline]] static inline StereoFrame ReadSource(VoiceSampleState& v,
                                                                uint32_t& frame,
                                                                bool& ended) {
        const uint32_t last_valid_frame = v.end_frame - 1;
        const uint32_t loop_len = v.loop_end - v.loop_start;
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
                    idx0 = v.sample_frames - 2;  // clamp: envelope release masks the tail anyway
                idx1 = idx0 + 1;
            }
            frac = v.phase.Fraction();
        }
        frame = idx0;
        ended = holding_release_tail;
        if (v.src_channels == 2) {
            const float l0 = static_cast<float>(v.sample[idx0 * 2]) / 32768.f;
            const float r0 = static_cast<float>(v.sample[idx0 * 2 + 1]) / 32768.f;
            const float l1 = static_cast<float>(v.sample[idx1 * 2]) / 32768.f;
            const float r1 = static_cast<float>(v.sample[idx1 * 2 + 1]) / 32768.f;
            if (v.stereo)
                return {l0 + (l1 - l0) * frac, r0 + (r1 - r0) * frac};
            const float s0 = (l0 + r0) * .5f, s1 = (l1 + r1) * .5f;
            const float mono = s0 + (s1 - s0) * frac;
            return {mono, mono};
        }
        const float s0 = static_cast<float>(v.sample[idx0]) / 32768.f;
        const float s1 = static_cast<float>(v.sample[idx1]) / 32768.f;
        const float mono = s0 + (s1 - s0) * frac;
        return {mono, mono};
    }

    /// Optional per-track mixer; nullptr means the pre-mixer behaviour.
    /// Not owned - see SetTrackMixer().
    const WaveX::Mix::TrackMixer* track_mixer_ = nullptr;
    static uint8_t TriggerChannels(const VoiceTriggerParams& params) {
        return SourceChannels(params) == 2 || SourceChannels(params.secondary) == 2 ? 2 : 1;
    }
    static uint8_t SourceChannels(const VoiceSampleParams& source) {
        return source.sample && source.sample_frames >= 2 && source.channels == 2 && !source.mono
                   ? 2
                   : 1;
    }
    std::array<Voice, WAVEX_NUM_VOICES> voices_{};
    NotePitchTable note_pitch_;
    std::array<uint32_t, WaveX::Mix::kNumTracks> track_bindings_{};
    uint64_t next_group_id_ = 1;  // zero after wrap refuses further admissions
    uint32_t next_age_ = 0;
    uint32_t sample_rate_ = 48000;
    uint64_t frame_clock_ = 0, beat_clock_ = 0;
    uint32_t beat_step_ = 0;
    float tempo_bpm_ = 120;
    // Live transpose as a rate multiplier. 1.0 until something moves PARAM_PITCH,
    // so a voice triggered before any edit sounds exactly as it did before.
    float VoicePitchScale(const Voice& voice) const {
        return (voice.param_lock_mask & VoiceLockBit(Protocol::PARAM_PITCH))
                   ? voice.locked_pitch_scale
                   : LivePitchScale(voice.track);
    }
    float LivePitchScale(uint8_t track) const {
        return track < live_pitch_scales_.size() ? live_pitch_scales_[track] : 1.0f;
    }
    std::array<float, WaveX::Mix::kNumTracks> live_pitch_scales_{};
    // SRC_RANDOM sample-and-hold seed (lfo.hpp's xorshift idiom), advanced
    // once per Trigger(). Non-zero default MUST also be set in Init() -
    // see that function's comment.
    uint32_t rng_ = 0x2545F491u;
};

}  // namespace AudioEngine
}  // namespace WaveX
