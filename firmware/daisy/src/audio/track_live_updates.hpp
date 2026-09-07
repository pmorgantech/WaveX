#pragma once

#include "snapshot_mailbox.hpp"
#include "voice_manager.hpp"
#include <array>

namespace WaveX {
namespace AudioEngine {

// Main owns instrument/extras state and publishes complete per-Track values.
// The callback alone applies them to sounding voices. Coalescing is scoped to
// one Track: an edit to another Track cannot replace an unconsumed update.
class TrackLiveUpdates {
   public:
    void Init() {
        for (uint8_t track = 0; track < Mix::kNumTracks; ++track) {
            VoiceLiveParams initial;
            initial.track = track;
            mailboxes_[track].Init(initial);
        }
    }

    void Publish(const VoiceLiveParams& params) {
        if (params.track < mailboxes_.size()) {
            mailboxes_[params.track].Publish(params);
        }
    }

    void ApplyTo(VoiceManager& voices) {
        VoiceLiveParams params;
        for (auto& mailbox: mailboxes_) {
            if (mailbox.ConsumeLatest(params)) {
                voices.ApplyLiveParams(params);
            }
        }
    }

   private:
    std::array<SnapshotMailbox<VoiceLiveParams>, Mix::kNumTracks> mailboxes_;
};

}  // namespace AudioEngine
}  // namespace WaveX
