#pragma once
#include "spi_protocol/protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wavex_ui {
// UI-owned confirmed control values. Correlation and freshness authorize edits;
// retained display values alone never do. No default Instrument is invented.
class PlayControlsModel {
   public:
    void Reset(uint8_t track) {
        *this = PlayControlsModel{};
        track_ = track;
    }
    uint8_t Track() const { return track_; }
    bool Due(unsigned group, uint32_t now) const {
        const auto& state = groups_[group];
        return state.dirty || now - state.sent_at >= (state.request ? 750u : 500u);
    }
    void Requested(unsigned group, uint32_t id, uint32_t now) {
        auto& state = groups_[group];
        state.request = id;
        state.sent_at = now;
        state.dirty = false;
    }
    bool Accept(const WaveX::Protocol::InstEditSyncMessage& reply, uint32_t now) {
        if (!Matches(0, reply.request_id, reply.track))
            return false;
        auto& state = groups_[0];
        state.request = 0;
        state.valid = reply.valid && !reply.busy && WaveX::Protocol::IsValidInstSound(reply.sound);
        if (!state.valid)
            return false;
        state.received_at = now;
        supported_[0] = reply.sound.cutoff_hz >= 20 && reply.sound.cutoff_hz <= 20000;
        supported_[1] = true;
        values_[0] =
            Wire(std::log(std::max(20.f, reply.sound.cutoff_hz) / 20.f) / std::log(1000.f));
        values_[1] = Wire(reply.sound.resonance);
        return true;
    }
    bool Accept(const WaveX::Protocol::InstModSyncMessage& reply, uint32_t now) {
        if (!Matches(1, reply.request_id, reply.track))
            return false;
        auto& state = groups_[1];
        state.request = 0;
        const auto& env = reply.envelopes[0];
        state.valid = reply.valid && !reply.busy && WaveX::Protocol::IsValidInstEnvelope(env);
        if (!state.valid)
            return false;
        state.received_at = now;
        const float fields[] = {env.attack_s, env.decay_s, env.sustain, env.release_s};
        for (unsigned i = 0; i < 4; ++i) {
            supported_[i + 2] = i == 2 || (fields[i] >= .001f && fields[i] <= 2.001f);
            values_[i + 2] = Wire(i == 2 ? fields[i] : (fields[i] - .001f) / 2.f);
        }
        return true;
    }
    bool Ready(unsigned parameter, uint32_t now) const {
        if (parameter >= 6)
            return true;
        const auto& state = groups_[parameter < 2 ? 0 : 1];
        return state.valid && supported_[parameter] && now - state.received_at < 1500;
    }
    uint16_t Value(unsigned parameter) const { return values_[parameter]; }
    void Edited(unsigned parameter) {
        auto& state = groups_[parameter < 2 ? 0 : 1];
        state.valid = false;
        state.request = 0;
        state.dirty = true;
    }

   private:
    struct Group {
        uint32_t request = 0, sent_at = 0, received_at = 0;
        bool valid = false, dirty = true;
    };
    bool Matches(unsigned group, uint32_t id, uint8_t track) const {
        return id && id == groups_[group].request && track == track_;
    }
    static uint16_t Wire(float value) {
        return static_cast<uint16_t>(std::lround(std::clamp(value, 0.f, 1.f) * 65535.f));
    }
    uint8_t track_ = 0;
    std::array<Group, 2> groups_{};
    std::array<uint16_t, 6> values_{};
    std::array<bool, 6> supported_{};
};
}  // namespace wavex_ui
