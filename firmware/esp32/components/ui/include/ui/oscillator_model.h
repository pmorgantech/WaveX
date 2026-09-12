#pragma once
#include "spi_protocol/protocol.h"

#include <algorithm>
#include <cstring>

namespace wavex_ui {
// Backend snapshot plus a coalesced outgoing preview. The backend retains
// the Apply/Revert baseline; this model only tracks delivery.
class OscillatorModel {
    using State = WaveX::Protocol::InstOscSyncMessage;
    using Settings = WaveX::Protocol::InstOscSettings;

   public:
    void Reset(uint8_t track, uint8_t oscillator) {
        *this = OscillatorModel{};
        state_.track = track;
        state_.oscillator = oscillator;
    }
    void AdoptRevision(uint32_t revision) {
        if (valid_)
            state_.revision = revision;
    }
    void Expect(uint32_t id) { expected_ = id; }
    void MutationSent(uint32_t id) {
        pending_ = expected_ = id;
        sent_ = draft_;
    }
    bool Accept(const State& s) {
        if (valid_ && static_cast<int32_t>(s.revision - state_.revision) < 0)
            return false;
        if (!expected_ || s.request_id != expected_ || s.track != state_.track ||
            s.oscillator != state_.oscillator || !s.revision || s.valid > 1 || s.busy > 1 ||
            s.type > 2 || s.zones > 32 || s.reserved || s.value.reserved ||
            !(s.value.level >= 0 && s.value.level <= 64) ||
            !(s.value.mix >= 0 && s.value.mix <= 1) || s.value.keytrack > 1)
            return false;
        if (valid_ && std::memcmp(&state_, &s, sizeof(s)) == 0)
            return false;
        const bool completed = pending_ && s.completed_request_id == pending_;
        const bool replaced = valid_ && s.revision != state_.revision;
        if (dirty_ && replaced && !completed)
            conflict_ = true;
        const bool queued = pending_ && std::memcmp(&draft_, &sent_, sizeof(draft_)) != 0;
        state_ = s;
        valid_ = true;
        if (completed)
            pending_ = 0;
        if (completed && queued && !s.error) {
            dirty_ = std::memcmp(&draft_, &state_.value, sizeof(draft_)) != 0;
        } else if ((!dirty_ && !pending_) || replaced || completed) {
            draft_ = state_.value;
            dirty_ = false;
        }
        return true;
    }
    const State& Snapshot() const { return state_; }
    bool Valid() const { return valid_; }
    bool Ready() const { return valid_ && !state_.busy && !pending_; }
    bool Editable() const { return valid_ && !state_.busy && state_.valid && state_.type != 2; }
    bool Dirty() const { return dirty_; }
    bool Pending() const { return pending_ != 0; }
    bool Conflict() const { return conflict_; }
    void Revert() {
        if (pending_)
            return;
        draft_ = state_.value;
        dirty_ = conflict_ = false;
    }
    int Value(uint8_t field) const {
        switch (field) {
            case 0:
                return static_cast<int>(draft_.level * 1000 + 0.5f);
            case 1:
                return static_cast<int>(draft_.mix * 1000 + 0.5f);
            case 2:
                return draft_.coarse;
            case 3:
                return draft_.fine;
            case 4:
                return draft_.keytrack;
            default:
                return 0;
        }
    }
    bool Set(uint8_t field, int value) {
        if (!Editable() || field >= 5)
            return false;
        switch (field) {
            case 0:
                draft_.level = static_cast<float>(std::clamp(value, 0, 64000)) / 1000;
                break;
            case 1:
                draft_.mix = static_cast<float>(std::clamp(value, 0, 1000)) / 1000;
                break;
            case 2:
                draft_.coarse = static_cast<int8_t>(std::clamp(value, -128, 127));
                break;
            case 3:
                draft_.fine = static_cast<int8_t>(std::clamp(value, -128, 127));
                break;
            case 4:
                draft_.keytrack = value > 0;
                break;
        }
        dirty_ = std::memcmp(&draft_, &state_.value, sizeof(draft_)) != 0;
        conflict_ = false;
        return true;
    }
    WaveX::Protocol::InstOscOpMessage Request(uint32_t id, uint8_t op) const {
        WaveX::Protocol::InstOscOpMessage request;
        request.request_id = id;
        request.revision = state_.revision;
        request.track = state_.track;
        request.oscillator = state_.oscillator;
        request.source = 1 - state_.oscillator;
        request.op = op;
        request.value = draft_;
        return request;
    }

   private:
    State state_{};
    Settings draft_{}, sent_{};
    uint32_t expected_ = 0, pending_ = 0;
    bool valid_ = false, dirty_ = false, conflict_ = false;
};
}  // namespace wavex_ui
