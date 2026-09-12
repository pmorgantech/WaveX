#pragma once
#include "spi_protocol/protocol.h"

#include <algorithm>
#include <cstring>

namespace wavex_ui {
// The backend owns all settings. Only one envelope or matrix row is drafted.
class ModulatorModel {
    using State = WaveX::Protocol::InstModSyncMessage;

   public:
    void Reset(uint8_t track) {
        *this = ModulatorModel{};
        state_.track = track;
    }
    void Expect(uint32_t id) { expected_ = id; }
    void MutationSent(uint32_t id) { pending_ = expected_ = id; }
    bool Accept(const State& s) {
        if (!expected_ || s.request_id != expected_ || s.track != state_.track || !s.revision ||
            s.valid > 1 || s.busy > 1)
            return false;
        for (const auto& e: s.envelopes)
            if (!WaveX::Protocol::IsValidInstEnvelope(e))
                return false;
        // Preserve unknown matrix fields from newer files; only writes must
        // satisfy this firmware's supported contract.
        State previous = state_;
        previous.request_id = s.request_id;
        if (valid_ && std::memcmp(&previous, &s, sizeof(s)) == 0)
            return false;
        const bool completed = pending_ && s.completed_request_id == pending_;
        const bool replaced = valid_ && s.revision != state_.revision;
        if (dirty_ && replaced && !completed)
            conflict_ = true;
        state_ = s;
        valid_ = true;
        if (completed)
            pending_ = 0;
        if (!dirty_ || replaced || completed)
            LoadDraft();
        return true;
    }
    bool Select(bool envelope, uint8_t index) {
        if (dirty_ || pending_ || index >= (envelope ? 3 : 8))
            return false;
        envelope_ = envelope;
        index_ = index;
        LoadDraft();
        conflict_ = false;
        return true;
    }
    bool Envelope() const { return envelope_; }
    uint8_t Index() const { return index_; }
    const State& Snapshot() const { return state_; }
    bool Valid() const { return valid_; }
    bool Ready() const { return valid_ && !state_.busy && !pending_; }
    bool Editable() const { return Ready() && state_.valid; }
    bool Dirty() const { return dirty_; }
    bool Pending() const { return pending_ != 0; }
    bool Conflict() const { return conflict_; }
    void Revert() {
        if (!pending_) {
            LoadDraft();
            conflict_ = false;
        }
    }
    int Value(uint8_t field) const {
        if (envelope_) {
            float value = field == 0   ? env_.attack_s
                          : field == 1 ? env_.decay_s
                          : field == 2 ? env_.sustain
                                       : env_.release_s;
            return static_cast<int>(value * 1000 + .5f);
        }
        switch (field) {
            case 0:
                return slot_.source;
            case 1:
                return slot_.destination;
            case 2:
                return slot_.depth;
            case 3:
                return slot_.curve;
            case 4:
                return slot_.flags;
            default:
                return 0;
        }
    }
    bool Set(uint8_t field, int value) {
        if (!Editable())
            return false;
        if (envelope_) {
            if (field >= 4 || value < 0 || value > (field == 2 ? 1000 : 600000))
                return false;
            const float v = static_cast<float>(value) / 1000;
            if (field == 0)
                env_.attack_s = v;
            else if (field == 1)
                env_.decay_s = v;
            else if (field == 2)
                env_.sustain = v;
            else
                env_.release_s = v;
        } else {
            const int low = field == 2 ? -32767 : 0;
            const int high = field == 0   ? 16
                             : field == 1 ? 4
                             : field == 2 ? 32767
                             : field == 3 ? 2
                                          : 1;
            if (field >= 5 || value < low || value > high)
                return false;
            switch (field) {
                case 0:
                    slot_.source = static_cast<uint8_t>(value);
                    break;
                case 1:
                    slot_.destination = static_cast<uint8_t>(value);
                    break;
                case 2:
                    slot_.depth = static_cast<int16_t>(value);
                    break;
                case 3:
                    slot_.curve = static_cast<uint8_t>(value);
                    break;
                case 4:
                    slot_.flags = static_cast<uint8_t>(value);
                    break;
            }
        }
        UpdateDirty();
        return true;
    }
    bool Clear() {
        if (!Editable() || envelope_)
            return false;
        slot_ = {};
        UpdateDirty();
        return true;
    }
    WaveX::Protocol::InstModOpMessage Request(uint32_t id, bool get = false) const {
        WaveX::Protocol::InstModOpMessage r;
        r.request_id = id;
        r.revision = state_.revision;
        r.track = state_.track;
        r.index = index_;
        r.op = get         ? WaveX::Protocol::INST_MOD_GET
               : envelope_ ? WaveX::Protocol::INST_MOD_SET_ENV
                           : WaveX::Protocol::INST_MOD_SET_SLOT;
        r.envelope = env_;
        r.slot = slot_;
        return r;
    }

   private:
    void LoadDraft() {
        if (envelope_)
            env_ = state_.envelopes[index_];
        else
            slot_ = state_.slots[index_];
        dirty_ = false;
    }
    void UpdateDirty() {
        dirty_ = envelope_ ? std::memcmp(&env_, &state_.envelopes[index_], sizeof(env_)) != 0
                           : std::memcmp(&slot_, &state_.slots[index_], sizeof(slot_)) != 0;
        conflict_ = false;
    }
    State state_{};
    WaveX::Protocol::InstEnvelopeSettings env_{};
    WaveX::Protocol::InstModSlotSettings slot_{};
    uint32_t expected_ = 0, pending_ = 0;
    uint8_t index_ = 0;
    bool envelope_ = true, valid_ = false, dirty_ = false, conflict_ = false;
};
}  // namespace wavex_ui
