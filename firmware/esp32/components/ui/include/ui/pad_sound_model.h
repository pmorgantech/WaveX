#pragma once
#include "spi_protocol/protocol.h"

#include <array>
#include <cstdint>

namespace wavex_ui {
// UI-domain desired values stay separate from backend-authoritative readback.
// One mutation is in flight; subsequent drags coalesce by field.
class PadSoundModel {
    using Request = WaveX::Protocol::InstPadSoundOpMessage;
    using State = WaveX::Protocol::InstPadSoundSyncMessage;

   public:
    void Reset(uint8_t track, uint8_t pad) {
        *this = PadSoundModel{};
        state_.track = track;
        state_.pad = pad;
    }
    void Expect(uint32_t id) { expected_ = id; }
    bool Accept(const State& s) {
        if (s.request_id != expected_ || s.track != state_.track || s.pad != state_.pad ||
            s.valid > 1 || s.busy > 1 || s.own > 1 || s.cutoff_hz > 20000 || s.attack_ms > 10000 ||
            s.decay_ms > 10000 || s.sustain > 1000 || (s.valid && !s.sample_id))
            return false;
        if (valid_ && (!s.valid || s.sample_id != state_.sample_id)) {
            dirty_ = 0;
            pending_ = {};
            inherit_ = false;
        }
        state_ = s;
        valid_ = true;
        if (pending_.request_id && s.completed_request_id == pending_.request_id) {
            error_ = s.error;
            if (s.error) {
                dirty_ = 0;
                inherit_ = false;
            } else if (pending_.op == WaveX::Protocol::PAD_SOUND_INHERIT)
                inherit_ = false;
            else {
                const auto field = pending_.op - WaveX::Protocol::PAD_SOUND_CUTOFF;
                if (desired_[field] == pending_.value)
                    dirty_ &= static_cast<uint8_t>(~(1u << field));
            }
            pending_ = {};
        }
        return true;
    }
    bool Editable() const { return valid_ && state_.valid && !state_.busy && !inherit_; }
    bool Ready() const { return valid_ && !state_.busy && !Pending(); }
    bool Pending() const { return dirty_ || inherit_ || pending_.request_id; }
    bool Valid() const { return valid_; }
    uint8_t Error() const { return error_; }
    const State& Snapshot() const { return state_; }
    uint16_t Value(uint8_t field) const {
        if (field >= 4)
            return 0;
        if (dirty_ & (1u << field))
            return desired_[field];
        const uint16_t values[] = {
            state_.cutoff_hz, state_.attack_ms, state_.decay_ms, state_.sustain};
        return values[field];
    }
    bool Set(uint8_t field, uint16_t value) {
        if (field >= 4 || !Editable())
            return false;
        Request m{
            1, state_.track, state_.pad, static_cast<uint8_t>(field + 2), state_.sample_id, value};
        if (!WaveX::Protocol::IsValidPadSoundOp(m))
            return false;
        desired_[field] = value;
        dirty_ |= static_cast<uint8_t>(1u << field);
        error_ = 0;
        return true;
    }
    bool Inherit() {
        if (!Editable() || Pending())
            return false;
        inherit_ = true;
        error_ = 0;
        return true;
    }
    bool Next(uint32_t id, Request& out) {
        if (!valid_ || !state_.valid || state_.busy || pending_.request_id || !Pending())
            return false;
        out = {
            id, state_.track, state_.pad, WaveX::Protocol::PAD_SOUND_INHERIT, state_.sample_id, 0};
        if (!inherit_) {
            for (uint8_t i = 0; i < 4; ++i)
                if (dirty_ & (1u << i)) {
                    out.op = static_cast<uint8_t>(i + 2);
                    out.value = desired_[i];
                    break;
                }
        }
        pending_ = out;
        expected_ = id;
        return true;
    }
    void SendFailed() { pending_ = {}; }

   private:
    State state_{};
    Request pending_{};
    std::array<uint16_t, 4> desired_{};
    uint32_t expected_ = 0;
    uint8_t dirty_ = 0, error_ = 0;
    bool valid_ = false, inherit_ = false;
};
}  // namespace wavex_ui
