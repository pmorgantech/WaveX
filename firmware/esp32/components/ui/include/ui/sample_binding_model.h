#pragma once
#include "spi_protocol/protocol.h"

#include <cstdint>

namespace wavex_ui {
// One UI-owned bind after a successful load. Retry only rejected enqueues;
// after admission, poll read-only state. Navigation/link loss never replays it.
class SampleBindingModel {
   public:
    enum class State { Idle, Sending, Reading, Confirmed, Failed };
    void Begin(uint16_t sample, uint8_t track, uint32_t now) {
        *this = SampleBindingModel{};
        sample_ = sample;
        track_ = track;
        started_ = now;
        state_ = sample && track < 16 ? State::Sending : State::Failed;
    }
    void Cancel() {
        state_ = State::Idle;
        request_ = 0;
    }
    bool Active() const { return state_ == State::Sending || state_ == State::Reading; }
    State Status() const { return state_; }
    uint16_t Sample() const { return sample_; }
    uint8_t Track() const { return track_; }
    void Check(uint32_t now, bool alive) {
        if (Active() && (!alive || now - started_ >= 5000))
            state_ = State::Failed;
    }
    bool NeedSend(uint32_t now) const {
        return state_ == State::Sending && (!attempted_ || now - attempted_at_ >= 100);
    }
    void Sent(bool accepted, uint32_t now) {
        if (state_ != State::Sending)
            return;
        attempted_ = true;
        attempted_at_ = now;
        if (accepted) {
            state_ = State::Reading;
            attempted_ = false;
        }
    }
    bool NeedRead(uint32_t now) const {
        return state_ == State::Reading && (!attempted_ || now - attempted_at_ >= 750);
    }
    void Requested(uint32_t id, bool accepted, uint32_t now) {
        if (state_ != State::Reading)
            return;
        attempted_ = true;
        attempted_at_ = now;
        request_ = accepted ? id : 0;
    }
    bool Accept(const WaveX::Protocol::TrackStateMessage& reply) {
        if (state_ != State::Reading || !request_ || reply.request_id != request_ ||
            reply.track != track_)
            return false;
        request_ = 0;
        if (reply.busy)
            return false;
        state_ = reply.valid && reply.loaded && reply.sample_id == sample_ ? State::Confirmed
                                                                           : State::Failed;
        return true;
    }

   private:
    State state_ = State::Idle;
    uint16_t sample_ = 0;
    uint8_t track_ = 0;
    uint32_t started_ = 0, attempted_at_ = 0, request_ = 0;
    bool attempted_ = false;
};
}  // namespace wavex_ui
