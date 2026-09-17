#pragma once
#include "sequencer/pattern_exchange.hpp"
namespace WaveX::Storage {
// Foreground owner of arrangement edits and the immutable Project playback loan.
// The exchange release is required before the Project can be mutated or freed.
class ProjectSongs {
   public:
    explicit ProjectSongs(Sequencer::PatternExchange& exchange) : exchange_(exchange) {}
    bool Request(const Protocol::SeqSongOpMessage&, const Sequencer::Project*, bool external_busy);
    void Pump(Sequencer::Project*);
    bool Busy() const { return phase_ != Phase::Idle; }
    bool BlocksEdits() const { return phase_ == Phase::Capture; }
    bool ReplyPending() const { return reply_; }
    void ReplySent() { reply_ = false; }
    const Protocol::SeqSongStatusMessage& Status() const { return status_; }

   private:
    enum class Phase { Idle, Capture, Start, Playing };
    void Refresh(const Sequencer::Project*);
    void Finish(uint8_t error);
    Sequencer::PatternExchange& exchange_;
    Protocol::SeqSongOpMessage request_;
    Protocol::SeqSongStatusMessage status_;
    Phase phase_ = Phase::Idle;
    bool reply_ = false;
    uint8_t playback_song_ = 0;
};
}  // namespace WaveX::Storage
