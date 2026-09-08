#pragma once

// Prepared per-Track triggers. The foreground owns resolution against mutable
// Instruments and the Sample Pool; the callback receives a complete snapshot.
// Until melodic note lanes are implemented, every row plays MIDI note 60.
// Velocity-zone selection retains the existing fixed-resolution velocity;
// per-step velocity still controls the triggered voice's amplitude.

#include "audio/instrument.hpp"
#include "sequencer/pattern.hpp"

namespace WaveX {
namespace AudioEngine {

struct SequencerVoiceMap {
    static_assert(Sequencer::kMaxTracks == kNumTracks,
                  "Pattern rows address the same Tracks as Instruments");
    static constexpr uint8_t kTriggerNote = 60;
    static constexpr uint8_t kResolveVelocity = 127;

    uint8_t layer_count[Sequencer::kMaxTracks] = {};
    VoiceTriggerParams layers[Sequencer::kMaxTracks][kMaxLayerTriggers] = {};

    // Resolver follows SfzLoader::ResolveNote. Never invoke it in the callback.
    template <typename Resolver>
    void Rebuild(Resolver resolve, uint16_t unavailable_tracks) {
        for (uint8_t track = 0; track < Sequencer::kMaxTracks; ++track) {
            layer_count[track] = 0;
            if ((unavailable_tracks & (1u << track)) == 0) {
                const uint8_t count = resolve(
                    track, kTriggerNote, kResolveVelocity, layers[track], kMaxLayerTriggers);
                layer_count[track] = count > kMaxLayerTriggers ? kMaxLayerTriggers : count;
            }
        }
    }

    // Revoke references before requesting the corresponding voice-stop fence.
    // Unaffected rows retain their complete prepared bindings.
    void Revoke(uint16_t tracks) {
        for (uint8_t track = 0; track < Sequencer::kMaxTracks; ++track) {
            if ((tracks & (1u << track)) != 0) {
                layer_count[track] = 0;
            }
        }
    }
};

}  // namespace AudioEngine
}  // namespace WaveX
