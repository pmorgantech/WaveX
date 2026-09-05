#pragma once

// Instrument / preset model (roadmap Phase 2.5 item 1; design:
// docs/features/instrument-model.md §2-3). E-mu-lineage keymap: an
// Instrument is a set of Zones, each mapping a sample to a key range x
// velocity range with per-zone tune / gain / pan / region-loop / filter /
// ADSR / choke. HAL-free: the resolution logic below turns an incoming note
// into a set of VoiceTriggerParams (the voice_manager.hpp trigger struct),
// looking up the actual sample bytes through a caller-supplied SampleResolver
// (function pointer + context, not std::function - same C++14/no-heap
// reasoning as wxcf.hpp's IoContext). So zone matching, velocity
// switch/layer/crossfade, tuning fold, and drum-vs-keyboard behavior are all
// host-testable without SDRAM or the audio HAL.
//
// In scope here: the Zone/Instrument/Tracks data model and
// ResolveNoteOn(). Deliberately NOT here: the SampleResolver implementations
// (Sfz::SampleTable in sfz_import.hpp for imported instruments; the bridging
// resolver over the plain-WAV registry in audio_engine.cpp for instruments
// built on-device - which one a given Instrument uses is decided by its
// `origin`, see below), WXCF persistence, and the protocol ops. A "kit" is
// just a drum-mode Instrument (instrument-model.md §8), so the Phase 2
// sequencer's per-track sample lookup will resolve through exactly this path
// once wired.

#include "mod_matrix.hpp"
#include "voice_manager.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace WaveX {
namespace AudioEngine {

static constexpr uint8_t kMaxZones = 32;
static constexpr uint8_t kMaxLayerTriggers = 4;  // zones fired per note-on, cap
static constexpr uint8_t kNumTracks = 16;

enum class InstrumentMode : uint8_t {
    Keyboard = 0,  // zones pitch-track relative to root_note
    Drum = 1,      // one-per-key pads, no pitch tracking (note forced to root)
};

enum ZoneFlags : uint8_t {
    ZONE_FLAG_VEL_XFADE = 1 << 0,       // gain rises across the velocity span
    ZONE_FLAG_VEL_XFADE_DOWN = 1 << 1,  // gain falls across the velocity span
    ZONE_FLAG_ONE_SHOT = 1 << 2,        // ignore note-off (play to end)
    // Filter cutoff/resonance and the amp ADSR come from the engine's live
    // base params (VoiceLiveParams - what the Play page's knobs edit) at
    // trigger time, not from this zone's own fields. This is how an
    // instrument built on-device from a bare sample keeps sounding like the
    // sweep the user just heard: it has no zone editor yet, so the live
    // params ARE its settings. An imported SFZ zone carries its own values
    // and leaves this clear. A zone editor that writes cutoff_hz/ADSR into
    // the zone should clear it too, at which point the zone owns them.
    ZONE_FLAG_LIVE_FILTER_ENV = 1 << 3,
};

// Zone::loop_mode. `Inherit` is instrument-model.md §2's "0 = use the sample's
// own marker": the loop comes from the SampleRef the sample_id resolves to
// (a bare WAV's sidecar loop points, for example). An imported SFZ region
// resolves through a table whose refs carry no markers, so for it Inherit and
// Off are the same thing.
enum ZoneLoopMode : uint8_t {
    ZONE_LOOP_INHERIT = 0,
    ZONE_LOOP_FORWARD = 1,
    ZONE_LOOP_OFF = 2,
};

struct Zone {
    uint16_t sample_id = 0;
    uint8_t key_lo = 0, key_hi = 127;  // inclusive MIDI note range
    uint8_t vel_lo = 1, vel_hi = 127;  // inclusive velocity range
    uint8_t root_note = 60;
    int8_t coarse_tune = 0;  // semitones
    int8_t fine_tune = 0;    // cents
    float gain = 1.0f;       // linear, pre-velocity
    float pan = 0.5f;
    // Region/loop. 0 = use the resolved sample's own value (SampleRef below);
    // nonzero is a per-zone override of it.
    uint32_t start_frame = 0, end_frame = 0;
    uint32_t loop_start = 0, loop_end = 0;
    uint8_t loop_mode = ZONE_LOOP_INHERIT;  // ZoneLoopMode
    uint8_t choke_group = 0;                // 0=none, 1..N
    uint8_t output_bus = 0;                 // 0=stereo mix; Stage B: 1+group
    uint8_t flags = 0;                      // ZoneFlags
    float cutoff_hz = 20000.0f;
    float attack_s = 0.001f, decay_s = 0.05f, sustain = 0.8f, release_s = 0.1f;
    bool in_use = false;
};

// Where an Instrument's zones came from, which is also WHICH sample-id
// registry their `sample_id`s index. An SFZ import's ids are private to that
// import (Sfz::BuildSamplePlan numbers them 1..N per load); an instrument
// built on-device references the plain-WAV registry's ids
// (MSG_SAMPLE_LOAD's sample_id). The two id spaces overlap, so the origin is
// what lets a caller pick the right SampleResolver rather than guess.
enum class InstrumentOrigin : uint8_t {
    None = 0,       // nothing bound: every note on this Track drops
    SfzImport = 1,  // zones from an .sfz; ids index the loader's SampleTable
    Built = 2,      // zones synthesised on-device; ids index the WAV registry
};

// How many bytes of an instrument's display name travel to the frontend.
// Matches TrackBindingMessage::name, so the two cannot drift apart.
static constexpr uint8_t kInstrumentNameBytes = 24;

struct Instrument {
    InstrumentMode mode = InstrumentMode::Keyboard;
    InstrumentOrigin origin = InstrumentOrigin::None;
    // What to call this instrument on screen: an import's .sfz basename, set
    // at load. Empty for a Built instrument, whose name is the bound sample's
    // own metadata and already known to the frontend. Without this the UI can
    // only say "Instrument bound" and never which Instrument.
    char name[kInstrumentNameBytes] = {};
    Zone zones[kMaxZones];
    // Modulation matrix (param-locks-and-modulation.md §3/§9 stage 4).
    // Always kMaxModSlots (8) entries - there is no separate "how many are
    // populated" count, because a default-constructed ModSlot is already
    // SRC_NONE/DEST_NONE, which EvaluateModMatrix() treats as a no-op. An
    // instrument nothing has configured therefore modulates nothing.
    ModSlot mod_slots[kMaxModSlots];
};

// The loaded audio a zone's sample_id resolves to. `valid()` gates whether a
// matching zone actually produces a voice.
//
// The trailing fields are the sample's OWN playback record - the region,
// loop and gain its sidecar markers resolve to - which a zone inherits
// wherever it leaves its corresponding field at 0 (instrument-model.md §2).
// A resolver with no such record (an SFZ import's table) leaves them at the
// defaults, which mean "whole sample, no loop, unity", exactly as before.
struct SampleRef {
    const int16_t* data = nullptr;
    uint32_t frames = 0;
    uint8_t channels = 1;
    uint32_t sample_rate_hz = 0;
    uint32_t start_frame = 0;
    uint32_t end_frame = 0;  // exclusive; 0 => frames
    bool loop_enabled = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;  // exclusive; 0 => end_frame
    uint16_t fade_in_ms = 0;
    uint16_t fade_out_ms = 0;
    float gain_mul = 1.0f;  // linear; composes with the zone's gain
    bool valid() const { return data != nullptr && frames >= 2; }
};

struct SampleResolver {
    const void* ctx = nullptr;
    SampleRef (*resolve)(const void* ctx, uint16_t sample_id) = nullptr;

    SampleRef Get(uint16_t sample_id) const {
        return resolve ? resolve(ctx, sample_id) : SampleRef{};
    }
};

// Coarse (semitones) + fine (cents) folded into a single pitch multiplier
// that composes onto VoiceManager's note/root 12-TET ratio.
inline float TuneRatio(int8_t coarse_semitones, int8_t fine_cents) {
    const float semitones =
        static_cast<float>(coarse_semitones) + static_cast<float>(fine_cents) / 100.0f;
    return std::pow(2.0f, semitones / 12.0f);
}

// Velocity crossfade (instrument-model.md §3.2): 1.0 unless a fade flag is
// set, then a linear ramp across the zone's velocity span. Two overlapping
// zones with opposite ramps crossfade through the overlap. Pure trigger-time
// arithmetic.
inline float VelocityXfadeGain(const Zone& zone, uint8_t velocity) {
    const uint16_t span =
        static_cast<uint16_t>(zone.vel_hi >= zone.vel_lo ? (zone.vel_hi - zone.vel_lo + 1) : 1);
    if (zone.flags & ZONE_FLAG_VEL_XFADE) {
        const uint16_t pos = static_cast<uint16_t>(velocity - zone.vel_lo + 1);
        return static_cast<float>(pos) / static_cast<float>(span);
    }
    if (zone.flags & ZONE_FLAG_VEL_XFADE_DOWN) {
        const uint16_t pos = static_cast<uint16_t>(zone.vel_hi - velocity + 1);
        return static_cast<float>(pos) / static_cast<float>(span);
    }
    return 1.0f;
}

// Resolves an incoming note-on against `ins` into up to `max` (capped at
// kMaxLayerTriggers) VoiceTriggerParams, ready to hand to VoiceManager::
// Trigger(). Returns the count filled. A zone matches when it is in_use and
// the note+velocity fall in its ranges; overlapping ranges layer (both fire),
// non-overlapping velocity ranges switch. Zones whose sample doesn't resolve
// are skipped (they don't consume a layer slot). Pure function - no state, no
// allocation, no I/O; safe to call from the audio-callback note path.
//
// `live` is the engine's current base params, read only by zones flagged
// ZONE_FLAG_LIVE_FILTER_ENV; nullptr makes such zones fall back to their own
// fields, so a caller with no live state loses nothing but that behaviour.
inline uint8_t ResolveNoteOn(const Instrument& ins,
                             uint8_t track,
                             uint8_t note,
                             uint8_t velocity,
                             const SampleResolver& resolver,
                             VoiceTriggerParams* out,
                             uint8_t max,
                             const VoiceLiveParams* live = nullptr) {
    if (!out || max == 0)
        return 0;
    if (max > kMaxLayerTriggers)
        max = kMaxLayerTriggers;

    uint8_t count = 0;
    for (uint8_t z = 0; z < kMaxZones && count < max; ++z) {
        const Zone& zone = ins.zones[z];
        if (!zone.in_use)
            continue;
        if (note < zone.key_lo || note > zone.key_hi)
            continue;
        if (velocity < zone.vel_lo || velocity > zone.vel_hi)
            continue;

        SampleRef ref = resolver.Get(zone.sample_id);
        if (!ref.valid())
            continue;

        VoiceTriggerParams p;
        p.sample = ref.data;
        p.sample_frames = ref.frames;
        p.channels = ref.channels;
        p.sample_rate_hz = ref.sample_rate_hz;
        p.note = (ins.mode == InstrumentMode::Drum) ? zone.root_note : note;
        p.trigger_note = note;
        p.velocity = velocity;
        p.root_note = zone.root_note;
        p.pan = zone.pan;
        p.track = track;
        p.choke_group = zone.choke_group;
        p.one_shot = (zone.flags & ZONE_FLAG_ONE_SHOT) != 0;

        p.gain_mul = zone.gain * VelocityXfadeGain(zone, velocity) * ref.gain_mul;
        p.pitch_ratio_mul = TuneRatio(zone.coarse_tune, zone.fine_tune);

        // Region/loop: the zone's value where it sets one, otherwise the
        // sample's own record. Fades have no zone field yet, so they are
        // always the sample's.
        p.start_frame = zone.start_frame ? zone.start_frame : ref.start_frame;
        p.end_frame = zone.end_frame ? zone.end_frame : ref.end_frame;
        p.loop = (zone.loop_mode == ZONE_LOOP_FORWARD) ||
                 (zone.loop_mode == ZONE_LOOP_INHERIT && ref.loop_enabled);
        p.loop_start = zone.loop_start ? zone.loop_start : ref.loop_start;
        p.loop_end = zone.loop_end ? zone.loop_end : ref.loop_end;
        p.fade_in_ms = ref.fade_in_ms;
        p.fade_out_ms = ref.fade_out_ms;

        if ((zone.flags & ZONE_FLAG_LIVE_FILTER_ENV) && live) {
            p.filter_cutoff_hz = live->filter_cutoff_hz;
            p.filter_resonance = live->filter_resonance;
            p.attack_s = live->attack_s;
            p.decay_s = live->decay_s;
            p.sustain_level = live->sustain_level;
            p.release_s = live->release_s;
        } else {
            p.filter_cutoff_hz = zone.cutoff_hz;
            p.attack_s = zone.attack_s;
            p.decay_s = zone.decay_s;
            p.sustain_level = zone.sustain;
            p.release_s = zone.release_s;
        }

        out[count++] = p;
    }
    return count;
}

// The sixteen Tracks. A sequencer track / MIDI channel addresses one; the
// Instrument bound to it resolves the note. Fixed storage, no allocation.
class Tracks {
   public:
    Instrument& Track(uint8_t track) { return tracks_[track < kNumTracks ? track : 0]; }
    const Instrument& Track(uint8_t track) const { return tracks_[track < kNumTracks ? track : 0]; }

    uint8_t ResolveNote(uint8_t track,
                        uint8_t note,
                        uint8_t velocity,
                        const SampleResolver& resolver,
                        VoiceTriggerParams* out,
                        uint8_t max,
                        const VoiceLiveParams* live = nullptr) const {
        if (track >= kNumTracks)
            return 0;
        return ResolveNoteOn(tracks_[track], track, note, velocity, resolver, out, max, live);
    }

   private:
    Instrument tracks_[kNumTracks];
};

}  // namespace AudioEngine
}  // namespace WaveX
