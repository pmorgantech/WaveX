#pragma once

#include "snapshot_mailbox.hpp"
#include <array>
#include <cstdint>

namespace WaveX::AudioEngine {
struct MidiModulationValues {
    float wheel = 0;
    float pressure = 0;
};
using MidiModulationSnapshot = std::array<MidiModulationValues, 16>;

// Foreground owns pending values and resolves the Track mask from current MIDI
// routing. The callback acquires one immutable latest-value snapshot per block.
// Bursts coalesce without consuming note/clock queue capacity. Omni is last
// matching event wins; values persist until reset, rebind or routing change.
class MidiModulation {
   public:
    void Init() {
        pending_ = {};
        mailbox_.Init(pending_);
    }
    void Wheel(uint16_t tracks, uint8_t value) { Set(tracks, value, false); }
    void Pressure(uint16_t tracks, uint8_t value) { Set(tracks, value, true); }
    void Reset(uint16_t tracks) {
        if (!tracks)
            return;
        for (std::size_t t = 0; t < pending_.size(); ++t)
            if (tracks & (1u << t))
                pending_[t] = {};
        mailbox_.Publish(pending_);
    }
    const MidiModulationSnapshot& Acquire() {
        mailbox_.AcquireLatest();
        return mailbox_.ConsumerValue();
    }

   private:
    void Set(uint16_t tracks, uint8_t value, bool pressure) {
        if (!tracks || value > 127)
            return;
        const float normalized = float(value) / 127.f;
        for (std::size_t t = 0; t < pending_.size(); ++t) {
            if (!(tracks & (1u << t)))
                continue;
            (pressure ? pending_[t].pressure : pending_[t].wheel) = normalized;
        }
        mailbox_.Publish(pending_);
    }
    MidiModulationSnapshot pending_{};
    SnapshotMailbox<MidiModulationSnapshot> mailbox_;
};
}  // namespace WaveX::AudioEngine
