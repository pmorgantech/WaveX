#pragma once

// Immutable prepared zones: foreground alone resolves Pool references and
// tuning. Single-zone drum pads use a compact index; other Instruments scan
// at most 32 compact zone keys. The callback copies at most
// four prepared triggers. It never reads foreground Instruments or the Pool.
#include "audio/instrument.hpp"
#include "sequencer/pattern.hpp"

namespace WaveX {
namespace AudioEngine {

struct SequencerVoiceMap {
    static_assert(Sequencer::kMaxTracks == kNumTracks, "Pattern rows address Tracks");
    struct Key {
        uint8_t key_lo = 0, key_hi = 0, vel_lo = 1, vel_hi = 127, flags = 0;
        bool drum = false;
    };
    // A small lookup window, not a limit on supported note ranges or zones.
    static constexpr uint8_t kDirectDrumKeys = 16;
    static constexpr uint8_t kNoZone = 0xFF;
    struct PreparedOscillator {
        uint8_t count = 0;
        bool direct_drum = false;
        uint8_t drum_base = 0;
        uint8_t drum_zones[kDirectDrumKeys]{};
        Key keys[kMaxZones]{};
        VoiceTriggerParams zones[kMaxZones]{};
    };
    // The base holds Oscillator 1's prepared map; Oscillator 2 is independent.
    struct PreparedTrack : PreparedOscillator {
        uint64_t revision = 0;  // foreground cache identity, not callback state
        PreparedOscillator secondary;
    };
    PreparedTrack tracks[kNumTracks]{};

    void PrepareTrack(uint8_t track, const Instrument& instrument, const SampleResolver& resolver) {
        if (track >= kNumTracks)
            return;
        auto& dest = tracks[track];
        ++dest.revision;
        PrepareOscillator(dest, instrument, resolver, track, 0);
        PrepareOscillator(dest.secondary, instrument, resolver, track, 1);
    }

    static void PrepareOscillator(PreparedOscillator& dest,
                                  const Instrument& instrument,
                                  const SampleResolver& resolver,
                                  uint8_t track,
                                  uint8_t osc) {
        dest.count = 0;
        dest.direct_drum = false;
        if (instrument.origin == InstrumentOrigin::None ||
            instrument.osc[osc].type != OscType::Sample)
            return;
        for (const auto& zone: instrument.osc[osc].zones) {
            if (!zone.in_use)
                continue;
            const auto sample = resolver.Get(zone.sample_id);
            if (!sample.valid())
                continue;
            const auto index = dest.count++;
            dest.keys[index] = {
                zone.key_lo,
                zone.key_hi,
                zone.vel_lo,
                zone.vel_hi,
                zone.flags,
                instrument.mode == InstrumentMode::Drum || !instrument.osc[osc].keytrack};
            dest.zones[index] =
                PrepareZoneTrigger(instrument, zone, sample, track, zone.root_note, 127, osc);
        }
        // Derive only when every prepared zone owns one distinct note in a
        // compact window. Velocity bounds/fades still use the zone key.
        if (instrument.mode != InstrumentMode::Drum || dest.count == 0)
            return;
        uint8_t low = 127, high = 0;
        for (uint8_t i = 0; i < dest.count; ++i) {
            const auto& key = dest.keys[i];
            if (key.key_lo != key.key_hi)
                return;
            low = key.key_lo < low ? key.key_lo : low;
            high = key.key_hi > high ? key.key_hi : high;
        }
        if (high - low >= kDirectDrumKeys)
            return;
        for (auto& index: dest.drum_zones)
            index = kNoZone;
        for (uint8_t i = 0; i < dest.count; ++i) {
            auto& index = dest.drum_zones[dest.keys[i].key_lo - low];
            if (index != kNoZone)
                return;  // layers and velocity splits retain ordered scanning
            index = i;
        }
        dest.drum_base = low;
        dest.direct_drum = true;
    }

    uint8_t Resolve(uint8_t track,
                    uint8_t note,
                    uint8_t velocity,
                    VoiceTriggerParams* out,
                    uint8_t max = kMaxLayerTriggers) const {
        if (track >= kNumTracks || note > 127 || velocity == 0 || velocity > 127 || !out)
            return 0;
        if (max > kMaxLayerTriggers)
            max = kMaxLayerTriggers;
        const auto& source = tracks[track];
        const auto first = ResolveOscillator(source, note, velocity, out, max);
        if (source.secondary.count == 0)
            return first;
        VoiceTriggerParams secondary[kMaxLayerTriggers];
        const auto second = ResolveOscillator(source.secondary, note, velocity, secondary, max);
        for (uint8_t i = 0; i < second; ++i) {
            if (i < first)
                PairOscillatorTrigger(out[i], secondary[i]);
            else
                out[i] = secondary[i];
        }
        return first > second ? first : second;
    }

    static WAVEX_ITCM_CODE_NAMED("resolve") uint8_t
        ResolveOscillator(const PreparedOscillator& source,
                          uint8_t note,
                          uint8_t velocity,
                          VoiceTriggerParams* out,
                          uint8_t max) {
        uint8_t begin = 0;
        uint8_t end = source.count < kMaxZones ? source.count : kMaxZones;
        if (source.direct_drum) {
            if (note < source.drum_base || note - source.drum_base >= kDirectDrumKeys)
                return 0;
            begin = source.drum_zones[note - source.drum_base];
            if (begin >= end)
                return 0;
            end = begin + 1;
        }
        uint8_t count = 0;
        for (uint8_t i = begin; i < end && count < max; ++i) {
            const auto& key = source.keys[i];
            if (note < key.key_lo || note > key.key_hi || velocity < key.vel_lo ||
                velocity > key.vel_hi)
                continue;
            auto& p = out[count++];
            p = source.zones[i];
            p.note = key.drum ? p.root_note : note;
            p.trigger_note = note;
            p.velocity = velocity;
            Zone fade;
            fade.vel_lo = key.vel_lo;
            fade.vel_hi = key.vel_hi;
            fade.flags = key.flags;
            p.gain_mul *= VelocityXfadeGain(fade, velocity);
        }
        return count;
    }

    // Foreground sparse copy: unused capacity is not live state. Copying all
    // 512 zone slots on every cutoff edit needlessly churns the SDRAM/cache
    // shared with audio. Per-Track revisions skip unchanged rows when a
    // mailbox slot is reused; PrepareTrack/Revoke are the foreground writers.
    // Counts hide old capacity in reused mailbox slots.
    void CopyLiveFrom(const SequencerVoiceMap& source) {
        for (uint8_t track = 0; track < kNumTracks; ++track) {
            auto& dest = tracks[track];
            const auto& src = source.tracks[track];
            if (src.revision != 0 && dest.revision == src.revision)
                continue;
            dest.revision = src.revision;
            CopyOscillator(dest, src);
            CopyOscillator(dest.secondary, src.secondary);
        }
    }

    static void CopyOscillator(PreparedOscillator& dest, const PreparedOscillator& src) {
        dest.count = src.count < kMaxZones ? src.count : kMaxZones;
        dest.direct_drum = src.direct_drum;
        dest.drum_base = src.drum_base;
        if (src.direct_drum)
            for (uint8_t i = 0; i < kDirectDrumKeys; ++i)
                dest.drum_zones[i] = src.drum_zones[i];
        for (uint8_t i = 0; i < dest.count; ++i) {
            dest.keys[i] = src.keys[i];
            dest.zones[i] = src.zones[i];
        }
    }

    void Revoke(uint16_t mask) {
        for (uint8_t track = 0; track < kNumTracks; ++track)
            if ((mask & (1u << track)) &&
                (tracks[track].count != 0 || tracks[track].secondary.count != 0)) {
                ++tracks[track].revision;
                tracks[track].count = 0;
                tracks[track].secondary.count = 0;
            }
    }
};

}  // namespace AudioEngine
}  // namespace WaveX
