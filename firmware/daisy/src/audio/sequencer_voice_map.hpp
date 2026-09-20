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
        uint8_t channels = 1, choke = 0;
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
        Allocation::Policy policy;
        bool keyboard = true;
    };
    PreparedTrack tracks[kNumTracks]{};

    void PrepareTrack(uint8_t track, const Instrument& instrument, const SampleResolver& resolver) {
        if (track >= kNumTracks)
            return;
        auto& dest = tracks[track];
        ++dest.revision;
        dest.policy = instrument.allocation;
        dest.keyboard = instrument.mode == InstrumentMode::Keyboard;
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
            const auto& trigger = dest.zones[index];
            dest.keys[index].channels = trigger.channels == 2 && !trigger.mono ? 2 : 1;
            dest.keys[index].choke = trigger.choke_group;
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

    // Indices borrow the acquired immutable map for this callback only. No
    // sample/DSP copies or velocity-fade arithmetic are needed for admission.
    struct Selection {
        uint8_t track = 0, note = 0, velocity = 0, count = 0;
        uint8_t zones[2][kMaxLayerTriggers]{};
    };

    WAVEX_ITCM_CODE_NAMED("resolve.Select")
    Selection Select(uint8_t track,
                     uint8_t note,
                     uint8_t velocity,
                     uint8_t max = kMaxLayerTriggers) const {
        Selection selected;
        if (track >= kNumTracks || note > 127 || velocity == 0 || velocity > 127)
            return selected;
        selected.track = track;
        selected.note = note;
        selected.velocity = velocity;
        max = std::min(max, kMaxLayerTriggers);
        for (auto& oscillator: selected.zones)
            for (auto& index: oscillator)
                index = kNoZone;
        for (uint8_t osc = 0; osc < 2; ++osc) {
            const auto& source = Oscillator(track, osc);
            uint8_t begin = 0, end = std::min(source.count, kMaxZones);
            if (source.direct_drum) {
                if (note < source.drum_base || note - source.drum_base >= kDirectDrumKeys)
                    continue;
                begin = source.drum_zones[note - source.drum_base];
                if (begin >= end)
                    continue;
                end = begin + 1;
            }
            uint8_t count = 0;
            for (uint8_t i = begin; i < end && count < max; ++i) {
                const auto& key = source.keys[i];
                if (note >= key.key_lo && note <= key.key_hi && velocity >= key.vel_lo &&
                    velocity <= key.vel_hi)
                    selected.zones[osc][count++] = i;
            }
            selected.count = std::max(selected.count, count);
        }
        return selected;
    }

    TriggerDescription Describe(const Selection& selected) const {
        TriggerDescription result;
        result.track = selected.track;
        result.policy = tracks[selected.track].policy;
        result.count = selected.count;
        for (uint8_t i = 0; i < selected.count; ++i) {
            const bool primary = selected.zones[0][i] != kNoZone;
            const auto& key = Oscillator(selected.track, primary ? 0 : 1)
                                  .keys[selected.zones[primary ? 0 : 1][i]];
            auto& layer = result.layers[i];
            layer.channels = key.channels;
            layer.choke = key.choke;
            if (primary && selected.zones[1][i] != kNoZone)
                layer.channels =
                    std::max(layer.channels,
                             tracks[selected.track].secondary.keys[selected.zones[1][i]].channels);
        }
        return result;
    }

    WAVEX_ITCM_CODE_NAMED("resolve.Materialize")
    void Materialize(const Selection& selected, uint8_t layer, VoiceTriggerParams& out) const {
        const bool primary = selected.zones[0][layer] != kNoZone;
        const uint8_t osc = primary ? 0 : 1;
        const auto& source = Oscillator(selected.track, osc);
        const auto index = selected.zones[osc][layer];
        const auto& key = source.keys[index];
        out = source.zones[index];
        ApplyNote(out, key, selected.note);
        out.trigger_note = selected.note;
        out.velocity = selected.velocity;
        out.gain_mul *= FadeGain(key, selected.velocity);
        if (primary && selected.zones[1][layer] != kNoZone) {
            const auto& second = tracks[selected.track].secondary;
            const auto second_index = selected.zones[1][layer];
            const auto& p = second.zones[second_index];
            // Copy only oscillator fields; the first layer owns shared DSP.
            out.secondary = static_cast<const VoiceSampleParams&>(p);
            ApplyNote(out.secondary, second.keys[second_index], selected.note);
            const float gain = p.gain_mul * FadeGain(second.keys[second_index], selected.velocity);
            out.dry_level *= out.gain_mul;
            out.source_level *= out.gain_mul;
            out.secondary.dry_level *= gain;
            out.secondary.source_level *= gain;
            out.gain_mul = 1.f;
        }
    }

    uint8_t Resolve(uint8_t track,
                    uint8_t note,
                    uint8_t velocity,
                    VoiceTriggerParams* out,
                    uint8_t max = kMaxLayerTriggers) const {
        if (!out)
            return 0;
        const auto selected = Select(track, note, velocity, max);
        for (uint8_t i = 0; i < selected.count; ++i)
            Materialize(selected, i, out[i]);
        return selected.count;
    }

   private:
    const PreparedOscillator& Oscillator(uint8_t track, uint8_t osc) const {
        return osc == 0 ? static_cast<const PreparedOscillator&>(tracks[track])
                        : tracks[track].secondary;
    }
    static void ApplyNote(VoiceSampleParams& p, const Key& key, uint8_t note) {
        p.note = key.drum ? p.root_note : note;
        p.key_note = p.drum ? p.root_note : note;
    }
    static float FadeGain(const Key& key, uint8_t velocity) {
        Zone fade;
        fade.vel_lo = key.vel_lo;
        fade.vel_hi = key.vel_hi;
        fade.flags = key.flags;
        return VelocityXfadeGain(fade, velocity);
    }

   public:
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
            dest.policy = src.policy;
            dest.keyboard = src.keyboard;
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
