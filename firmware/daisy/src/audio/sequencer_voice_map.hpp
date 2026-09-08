#pragma once

// Immutable prepared zones: foreground alone resolves Pool references and
// tuning. The callback scans at most 32 compact zone keys and copies at most
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
    struct PreparedTrack {
        // Foreground-only cache identity. The callback never inspects it.
        uint64_t revision = 0;
        uint8_t count = 0;
        Key keys[kMaxZones]{};
        VoiceTriggerParams zones[kMaxZones]{};
    };
    PreparedTrack tracks[kNumTracks]{};

    void PrepareTrack(uint8_t track, const Instrument& instrument, const SampleResolver& resolver) {
        if (track >= kNumTracks)
            return;
        auto& dest = tracks[track];
        ++dest.revision;
        dest.count = 0;
        if (instrument.origin == InstrumentOrigin::None)
            return;
        for (const auto& zone: instrument.zones) {
            if (!zone.in_use)
                continue;
            const auto sample = resolver.Get(zone.sample_id);
            if (!sample.valid())
                continue;
            const auto index = dest.count++;
            dest.keys[index] = {zone.key_lo,
                                zone.key_hi,
                                zone.vel_lo,
                                zone.vel_hi,
                                zone.flags,
                                instrument.mode == InstrumentMode::Drum};
            dest.zones[index] =
                PrepareZoneTrigger(instrument, zone, sample, track, zone.root_note, 127);
        }
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
        uint8_t count = 0;
        for (uint8_t i = 0; i < source.count && i < kMaxZones && count < max; ++i) {
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
            dest.count = src.count < kMaxZones ? src.count : kMaxZones;
            for (uint8_t i = 0; i < dest.count; ++i) {
                dest.keys[i] = src.keys[i];
                dest.zones[i] = src.zones[i];
            }
        }
    }

    void Revoke(uint16_t mask) {
        for (uint8_t track = 0; track < kNumTracks; ++track)
            if ((mask & (1u << track)) && tracks[track].count != 0) {
                ++tracks[track].revision;
                tracks[track].count = 0;
            }
    }
};

}  // namespace AudioEngine
}  // namespace WaveX
