#pragma once
#include "spi_protocol/protocol.h"
namespace wavex_ui {
class InstrumentTagsModel {
   public:
    void Reset(uint8_t track) {
        *this = {};
        state_.track = track;
    }
    void Expect(uint32_t id) { expected_ = id; }
    bool Accept(const WaveX::Protocol::InstZoneSyncMessage& state) {
        if (!expected_ || state.request_id != expected_ || state.track != state_.track ||
            !state.revision || state.loaded > 1 || state.busy > 1)
            return false;
        const bool complete = pending_ && state.completed_request_id == pending_;
        const bool replaced = valid_ && state.revision != state_.revision;
        expected_ = 0;
        state_ = state;
        valid_ = true;
        if (complete)
            pending_ = 0;
        if (!dirty_ || complete || replaced)
            Revert();
        return true;
    }
    bool Ready() const { return valid_ && state_.loaded && !state_.busy && !pending_; }
    bool Pending() const { return pending_ != 0; }
    bool Dirty() const { return dirty_; }
    uint8_t Draft() const { return draft_; }
    const auto& State() const { return state_; }
    void Revert() {
        draft_ = state_.tags;
        dirty_ = false;
    }
    void Toggle(uint8_t tag) {
        if (!Ready() || tag >= 8)
            return;
        draft_ ^= WaveX::InstrumentTags::Mask(tag);
        dirty_ = draft_ != state_.tags;
    }
    WaveX::Protocol::InstOpMessage Request(uint32_t id) const {
        WaveX::Protocol::InstOpMessage request(
            id, state_.track, WaveX::Protocol::INST_OP_SET_TAGS, "");
        request.tags = draft_;
        request.revision = state_.revision;
        return request;
    }
    void Sent(uint32_t id) { pending_ = id; }

   private:
    WaveX::Protocol::InstZoneSyncMessage state_;
    uint32_t expected_ = 0, pending_ = 0;
    uint8_t draft_ = 0;
    bool valid_ = false, dirty_ = false;
};
}  // namespace wavex_ui
