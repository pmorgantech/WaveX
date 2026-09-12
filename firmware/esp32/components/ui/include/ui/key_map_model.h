#pragma once
#include "spi_protocol/protocol.h"

#include <algorithm>
#include <cstring>
namespace wavex_ui {
// The backend owns the Instrument. This model owns one explicit, staged range
// edit; selecting another zone requires Apply/Revert, and a new revision drops
// stale local changes instead of applying them to a replacement Instrument.
class KeyMapModel {
    using State = WaveX::Protocol::InstKeyMapSyncMessage;
    using Zone = WaveX::Protocol::InstKeyZone;

   public:
    void Reset(uint8_t track, uint8_t oscillator = 0) {
        *this = KeyMapModel{};
        state_.track = track;
        oscillator_ = oscillator;
    }
    void Expect(uint32_t id) { expected_ = id; }
    void MutationSent(uint32_t id) { pending_ = id; }
    bool Accept(const State& s) {
        if (!expected_ || s.request_id != expected_ || s.track != state_.track || !s.revision ||
            s.loaded > 1 || s.mode > 1 || s.busy > 1 || !std::memchr(s.name, 0, sizeof(s.name)))
            return false;
        for (const auto& z: s.zones)
            if (!WaveX::Protocol::IsValidKeyZoneRange(z))
                return false;
        const bool complete = pending_ && s.completed_request_id == pending_;
        const bool replaced = valid_ && s.revision != state_.revision;
        if (dirty_ && replaced && !complete)
            conflict_ = true;
        state_ = s;
        valid_ = true;
        if (complete) {
            pending_ = 0;
            conflict_ = false;
        }
        if (!dirty_ || replaced || complete)
            Revert();
        return true;
    }
    bool Select(uint8_t zone) {
        if (zone >= WaveX::Protocol::INST_KEY_ZONE_COUNT || dirty_ || pending_)
            return false;
        selected_ = zone;
        Revert();
        return true;
    }
    void Revert() {
        draft_ = state_.zones[selected_];
        dirty_ = false;
    }
    bool Ready() const { return valid_ && !state_.busy && !pending_; }
    bool Editable() const { return Ready() && state_.loaded && state_.mode == 0; }
    bool Dirty() const { return dirty_; }
    bool Conflict() const { return conflict_; }
    uint8_t Selected() const { return selected_; }
    const State& Snapshot() const { return state_; }
    const Zone& Draft() const { return draft_; }
    uint8_t Value(uint8_t field) const {
        const uint8_t values[] = {
            draft_.key_lo, draft_.key_hi, draft_.vel_lo, draft_.vel_hi, draft_.root_note};
        return field < 5 ? values[field] : 0;
    }
    bool Set(uint8_t field, int value) {
        if (!Editable() || !draft_.sample_id || field >= 5)
            return false;
        const auto v =
            static_cast<uint8_t>(std::clamp(value, field == 2 || field == 3 ? 1 : 0, 127));
        switch (field) {
            case 0:
                draft_.key_lo = v;
                draft_.key_hi = std::max(v, draft_.key_hi);
                break;
            case 1:
                draft_.key_hi = v;
                draft_.key_lo = std::min(v, draft_.key_lo);
                break;
            case 2:
                draft_.vel_lo = v;
                draft_.vel_hi = std::max(v, draft_.vel_hi);
                break;
            case 3:
                draft_.vel_hi = v;
                draft_.vel_lo = std::min(v, draft_.vel_lo);
                break;
            case 4:
                draft_.root_note = v;
                break;
        }
        dirty_ = std::memcmp(&draft_, &state_.zones[selected_], sizeof(draft_)) != 0;
        conflict_ = false;
        return true;
    }
    WaveX::Protocol::InstKeyMapOpMessage Request(uint32_t id,
                                                 uint8_t op,
                                                 uint16_t sample = 0) const {
        WaveX::Protocol::InstKeyMapOpMessage m;
        m.request_id = id;
        m.revision = state_.revision;
        m.track = state_.track;
        m.oscillator = oscillator_;
        m.zone = selected_;
        m.op = op;
        m.expected_sample = state_.zones[selected_].sample_id;
        m.value = draft_;
        if (op == WaveX::Protocol::KEY_MAP_ASSIGN)
            m.value.sample_id = sample;
        return m;
    }

   private:
    State state_{};
    Zone draft_{};
    uint32_t expected_ = 0, pending_ = 0;
    uint8_t selected_ = 0, oscillator_ = 0;
    bool valid_ = false, dirty_ = false, conflict_ = false;
};
}  // namespace wavex_ui
