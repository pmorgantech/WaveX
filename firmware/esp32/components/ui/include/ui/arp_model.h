#pragma once
#include "spi_protocol/protocol.h"

#include <cstring>
namespace wavex_ui {
class ArpModel {
   public:
    void Reset(uint8_t track) {
        *this = ArpModel{};
        state_.track = track;
    }
    void AdoptRevision(uint32_t revision) {
        if (valid_)
            state_.revision = revision;
    }
    void Expect(uint32_t id) { expected_ = id; }
    void Sent(uint32_t id) {
        pending_ = expected_ = id;
        sent_ = desired_;
    }
    bool Accept(const WaveX::Protocol::InstArpSyncMessage& s) {
        if (!expected_ || s.request_id != expected_ || s.track != state_.track || !s.revision ||
            s.valid > 1 || s.busy > 1 || !WaveX::Arp::Valid(s.value) ||
            (valid_ && static_cast<int32_t>(s.revision - state_.revision) < 0))
            return false;
        auto previous = state_;
        previous.request_id = s.request_id;
        if (valid_ && std::memcmp(&previous, &s, sizeof(s)) == 0)
            return false;
        const bool complete = pending_ && s.completed_request_id == pending_;
        const bool queued = pending_ && !WaveX::Arp::Equal(desired_, sent_);
        const bool replaced = valid_ && s.revision != state_.revision;
        state_ = s;
        valid_ = true;
        if (complete)
            pending_ = 0;
        if (complete && queued && !s.error)
            dirty_ = !WaveX::Arp::Equal(desired_, s.value);
        else if (complete || replaced || (!pending_ && !dirty_)) {
            desired_ = s.value;
            dirty_ = false;
        }
        return true;
    }
    bool Ready() const { return valid_ && !state_.busy && !pending_; }
    bool Editable() const { return valid_ && !state_.busy && state_.valid; }
    bool Dirty() const { return dirty_; }
    bool Pending() const { return pending_ != 0; }
    bool Valid() const { return valid_; }
    const auto& Snapshot() const { return state_; }
    int Value(uint8_t field) const {
        switch (field) {
            case 0:
                return desired_.enabled;
            case 1:
                return desired_.mode;
            case 2:
                return desired_.division;
            case 3:
                return desired_.octaves;
            case 4:
                return desired_.gate_pct;
            case 5:
                return desired_.latch;
            case 6:
                return desired_.vel_mode;
            case 7:
                return desired_.vel_fixed;
            default:
                return 0;
        }
    }
    static int Minimum(uint8_t f) { return f == 3 || f == 4 || f == 7 ? 1 : 0; }
    static int Maximum(uint8_t f) {
        const int bounds[] = {1, 5, 11, 4, 100, 1, 2, 127};
        return f < 8 ? bounds[f] : 0;
    }
    bool Set(uint8_t field, int value) {
        if (!Editable() || field >= 8 || value < Minimum(field) || value > Maximum(field))
            return false;
        const auto v = static_cast<uint8_t>(value);
        switch (field) {
            case 0:
                desired_.enabled = v;
                break;
            case 1:
                desired_.mode = v;
                break;
            case 2:
                desired_.division = v;
                break;
            case 3:
                desired_.octaves = v;
                break;
            case 4:
                desired_.gate_pct = v;
                break;
            case 5:
                desired_.latch = v;
                break;
            case 6:
                desired_.vel_mode = v;
                break;
            case 7:
                desired_.vel_fixed = v;
                break;
        }
        dirty_ = !WaveX::Arp::Equal(desired_, state_.value);
        return true;
    }
    WaveX::Protocol::InstArpOpMessage Request(uint32_t id, bool get = false) const {
        WaveX::Protocol::InstArpOpMessage m;
        m.request_id = id;
        m.revision = state_.revision;
        m.track = state_.track;
        m.op = get ? WaveX::Protocol::INST_ARP_GET : WaveX::Protocol::INST_ARP_SET;
        m.value = desired_;
        return m;
    }

   private:
    WaveX::Protocol::InstArpSyncMessage state_;
    WaveX::Arp::Config desired_, sent_;
    uint32_t expected_ = 0, pending_ = 0;
    bool valid_ = false, dirty_ = false;
};
}  // namespace wavex_ui
