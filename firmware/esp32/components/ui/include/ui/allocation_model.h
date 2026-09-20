#pragma once
#include "spi_protocol/protocol.h"
namespace wavex_ui {
class AllocationModel {
   public:
    void Reset(uint8_t track, uint8_t scope) {
        state_ = {};
        state_.track = track;
        state_.scope = scope;
        expected_ = pending_ = 0;
        valid_ = false;
    }
    void Expect(uint32_t id) { expected_ = id; }
    bool Accept(const WaveX::Protocol::AllocationSyncMessage& s) {
        if (s.track != state_.track || s.scope != state_.scope ||
            !WaveX::Protocol::IsValidAllocationSync(s) || (!expected_ && !pending_) ||
            (s.request_id != expected_ && s.request_id != pending_))
            return false;
        // A read can overtake an edit acknowledgement. Keep the edit pending
        // until its retained completion is observed; never retry a mutation.
        if (valid_ && static_cast<int32_t>(s.revision - state_.revision) < 0)
            return false;
        state_ = s;
        valid_ = true;
        if (s.request_id == expected_)
            expected_ = 0;
        if (pending_ && s.completed_request_id == pending_)
            pending_ = 0;
        return true;
    }
    bool Begin(uint32_t id,
               uint8_t op,
               WaveX::Allocation::Policy value,
               bool inherit,
               WaveX::Protocol::AllocationOpMessage& out) {
        if (!Ready())
            return false;
        out = {};
        out.request_id = id;
        out.revision = state_.revision;
        out.track = state_.track;
        out.scope = state_.scope;
        out.op = op;
        out.policy = value;
        out.inherit = inherit;
        if (!WaveX::Protocol::IsValidAllocationOp(out))
            return false;
        pending_ = id;
        return true;
    }
    void SendFailed() { pending_ = 0; }
    bool Pending() const { return pending_ != 0; }
    bool Valid() const { return valid_; }
    bool Ready() const {
        return valid_ && state_.valid && state_.revision && !state_.busy && !pending_;
    }
    const WaveX::Protocol::AllocationSyncMessage& State() const { return state_; }
    WaveX::Allocation::Policy Effective() const {
        return state_.scope == WaveX::Protocol::ALLOC_SOUND || state_.inherited
                   ? state_.sound
                   : state_.track_policy;
    }

   private:
    WaveX::Protocol::AllocationSyncMessage state_;
    uint32_t expected_ = 0, pending_ = 0;
    bool valid_ = false;
};
}  // namespace wavex_ui
