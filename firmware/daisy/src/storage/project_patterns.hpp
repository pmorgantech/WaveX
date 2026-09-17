#pragma once
#include "sequencer/pattern_exchange.hpp"
#include "sequencer/project_data.hpp"

namespace WaveX::Storage {
// Foreground-only Project slot job. The session owns document lifetime and
// gates all competing mutations until Busy() clears. Never owns callback data.
class ProjectPatterns {
   public:
    explicit ProjectPatterns(Sequencer::PatternExchange& exchange) : exchange_(exchange) {}
    bool Request(const Protocol::SeqSlotOpMessage&,
                 const Sequencer::Project*,
                 bool external_busy,
                 uint8_t runtime_slot = 0xff);
    void Pump(Sequencer::Project*);
    void Refresh(const Sequencer::Project*, uint8_t runtime_slot = 0xff);
    void Fail(uint8_t error);
    bool BlocksEdits() const { return Busy() && phase_ != Phase::Launch; }
    bool Busy() const { return phase_ != Phase::Idle; }
    bool ReplyPending() const { return reply_; }
    void ReplySent() { reply_ = false; }
    const Protocol::SeqSlotStatusMessage& Status() const { return status_; }

   private:
    enum class Phase { Idle, Capture, Install, Launch };
    Sequencer::PatternExchange& exchange_;
    Protocol::SeqSlotOpMessage request_;
    Protocol::SeqSlotStatusMessage status_;
    Phase phase_ = Phase::Idle;
    bool reply_ = false;
};
}  // namespace WaveX::Storage
