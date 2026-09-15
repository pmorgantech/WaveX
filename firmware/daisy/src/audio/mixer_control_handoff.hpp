#pragma once

#include "spi_protocol/protocol.h"

#include "audio/track_mix.hpp"
#include "snapshot_mailbox.hpp"
#include <array>

namespace WaveX {
namespace AudioEngine {

// Main owns pending controls; callback owns TrackMixer, including ramp state.
// Publishing the complete table makes a solo/mute-mask transition indivisible
// and lets repeated edits coalesce without losing changes to other Tracks.
class MixerControlHandoff {
   public:
    void Init() {
        pending_ = Controls{};
        mailbox_.Init(pending_);
    }

    void Update(const Protocol::MixOpMessage& message) {
        using namespace Protocol;
        switch (message.op) {
            case MIX_OP_SET_GAIN:
            case MIX_OP_SET_PAN:
            case MIX_OP_SET_MUTE:
                if (message.track >= Mix::kNumTracks) {
                    return;
                }
                if (message.op == MIX_OP_SET_GAIN) {
                    pending_.tracks[message.track].gain =
                        Mix::DbToLinear(Mix::WireToGainDb(message.value));
                } else if (message.op == MIX_OP_SET_PAN) {
                    pending_.tracks[message.track].pan_offset = Mix::WireToPan(message.value);
                } else {
                    pending_.tracks[message.track].mute = message.value != 0;
                }
                break;
            case MIX_OP_SET_MUTE_MASK:
                for (uint8_t track = 0; track < Mix::kNumTracks; ++track) {
                    pending_.tracks[track].mute = ((message.value >> track) & 1u) != 0;
                }
                break;
            case MIX_OP_SET_SOLO_MASK:
                pending_.solo_mask = message.value;
                break;
            case MIX_OP_SET_MASTER:
                pending_.master_gain = Mix::DbToLinear(Mix::WireToGainDb(message.value));
                break;
            default:
                return;
        }
        mailbox_.Publish(pending_);
    }

    // Foreground only: read accepted targets without touching callback-owned ramps.
    Protocol::MixStateMessage Read(const Protocol::MixStateRequest& request) const {
        Protocol::MixStateMessage state{};
        state.request_id = request.request_id;
        state.track = request.track;
        if (!request.request_id || request.track >= Mix::kNumTracks)
            return state;
        const auto& strip = pending_.tracks[request.track];
        state.valid = 1;
        state.gain = Mix::GainDbToWire(Mix::LinearToDb(strip.gain));
        state.pan = static_cast<uint16_t>(std::lround((strip.pan_offset + 1.0f) * 32767.5f));
        state.mute = strip.mute;  // user mute, independent of temporary Solo
        return state;
    }

    void ApplyTo(Mix::TrackMixer& mixer) {
        Controls latest;
        if (!mailbox_.ConsumeLatest(latest)) {
            return;
        }
        const uint16_t solo_exclusions = Mix::ExpandSoloToMutes(latest.solo_mask, 0);
        for (uint8_t track = 0; track < Mix::kNumTracks; ++track) {
            mixer.SetGain(track, latest.tracks[track].gain);
            mixer.SetPanOffset(track, latest.tracks[track].pan_offset);
            const bool excluded_by_solo = (solo_exclusions & (1u << track)) != 0;
            mixer.SetMute(track, latest.tracks[track].mute || excluded_by_solo);
        }
        mixer.SetMasterGain(latest.master_gain);
    }

   private:
    struct Controls {
        std::array<Mix::TrackMix, Mix::kNumTracks> tracks;
        float master_gain = 1.0f;
        uint16_t solo_mask = 0;
    };
    Controls pending_;
    SnapshotMailbox<Controls> mailbox_;
};

}  // namespace AudioEngine
}  // namespace WaveX
