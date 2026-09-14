#pragma once
#include "spi_protocol/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
namespace wavex_ui {
// UI-task-owned delivery state. Audible values and the undo point belong to
// the backend. Rapid changes are coalesced without discarding the final value.
class InstrumentEditModel {
    using State = WaveX::Protocol::InstEditSyncMessage;
    using Sound = WaveX::Protocol::InstSoundSettings;

    // The Instrument-owned filter identity that travels beside the sound
    // values in INST_EDIT_FILTER_SETTINGS: mode, topology, slope, drive.
    struct Filter {
        uint8_t type = 0, topology = 0, slope = 0;
        float drive = 0;
        bool operator==(const Filter& o) const {
            return type == o.type && topology == o.topology && slope == o.slope && drive == o.drive;
        }
        bool operator!=(const Filter& o) const { return !(*this == o); }
        static Filter Of(const State& s) {
            return {s.filter_type, s.filter_topology, s.filter_slope, s.filter_drive};
        }
    };

   public:
    void Reset(uint8_t track) {
        *this = {};
        state_.track = track;
    }
    void AdoptRevision(uint32_t revision) {
        if (valid_)
            state_.revision = revision;
    }
    void Expect(uint32_t id) { expected_ = id; }
    const State& Snapshot() const { return state_; }
    bool Valid() const { return valid_; }
    bool Ready() const { return valid_ && !state_.busy && !pending_; }
    bool Editable() const { return valid_ && state_.valid && !state_.busy; }
    bool Pending() const { return pending_ != 0; }
    bool Outgoing() const { return outgoing_; }
    bool Dirty() const { return valid_ && state_.dirty; }
    uint8_t Operation() const { return operation_; }
    void Sent(uint32_t id, uint8_t op) {
        pending_ = expected_ = id;
        pending_op_ = op;
        sent_ = desired_;
        sent_filter_ = desired_filter_;
    }
    bool Accept(const State& s) {
        if (valid_ && static_cast<int32_t>(s.revision - state_.revision) < 0)
            return false;
        if (!expected_ || s.request_id != expected_ || s.track != state_.track || !s.revision ||
            s.valid > 1 || s.busy > 1 || s.dirty > 1 || s.reserved || s.reserved_tail[0] ||
            s.reserved_tail[1] || s.reserved_tail[2] ||
            s.filter_type > WaveX::Protocol::INST_FILTER_NOTCH ||
            s.filter_topology >= WaveX::Protocol::INST_FILTER_TOPOLOGY_COUNT ||
            s.filter_slope > WaveX::Protocol::INST_FILTER_SLOPE_24 ||
            !WaveX::Protocol::IsValidInstFilterDrive(s.filter_drive) ||
            !WaveX::Protocol::IsValidInstSound(s.sound))
            return false;
        State previous = state_;
        if (valid_ && std::memcmp(&previous, &s, sizeof(s)) == 0)
            return false;
        const bool completed = pending_ && s.completed_request_id == pending_;
        const bool queued = pending_ && (sent_filter_ != desired_filter_ ||
                                         std::memcmp(&sent_, &desired_, sizeof(Sound)) != 0);
        const bool replaced = valid_ && s.revision != state_.revision;
        state_ = s;
        valid_ = true;
        if (completed)
            pending_ = 0;
        if (completed && queued && !s.error && pending_op_ >= WaveX::Protocol::INST_EDIT_FILTER)
            outgoing_ = desired_filter_ != Filter::Of(s) ||
                        std::memcmp(&desired_, &s.sound, sizeof(Sound)) != 0;
        else if (completed || replaced || (!pending_ && !outgoing_)) {
            desired_ = s.sound;
            desired_filter_ = Filter::Of(s);
            outgoing_ = false;
        }
        return true;
    }
    // UI units: cutoff/resonance retain the existing 16-bit dial domain;
    // gain, pan and drive use thousandths and keep the saved Instrument range.
    // Fields: 0 cutoff, 1 resonance, 2 gain, 3 pan, 4 filter mode, 5 topology,
    // 6 slope (0 = 12 dB, 1 = 24 dB), 7 drive.
    int Value(uint8_t field) const {
        if (field == 7)
            return static_cast<int>(desired_filter_.drive * 1000 + .5f);
        if (field == 6)
            return desired_filter_.slope;
        if (field == 5)
            return desired_filter_.topology;
        if (field == 4)
            return desired_filter_.type;
        if (field == 0)
            return static_cast<int>(std::log(std::clamp(desired_.cutoff_hz, 20.f, 20000.f) / 20.f) /
                                        std::log(1000.f) * 65535.f +
                                    .5f);
        if (field == 1)
            return static_cast<int>(desired_.resonance * 65535 + .5f);
        if (field == 2)
            return static_cast<int>(desired_.gain * 1000 + .5f);
        return static_cast<int>(desired_.pan * 1000 + .5f);
    }
    bool Set(uint8_t field, int value) {
        if (!Editable() || field > 7 || value < 0 ||
            value > (field == 6   ? WaveX::Protocol::INST_FILTER_SLOPE_24
                     : field == 5 ? WaveX::Protocol::INST_FILTER_TOPOLOGY_COUNT - 1
                     : field == 4 ? 3
                     : field < 2  ? 65535
                     : field == 2 ? 64000
                                  : 1000))
            return false;
        if (field == 0)
            desired_.cutoff_hz = 20.f * std::pow(1000.f, value / 65535.f);
        if (field == 1)
            desired_.resonance = value / 65535.f;
        if (field == 2)
            desired_.gain = value / 1000.f;
        if (field == 3)
            desired_.pan = value / 1000.f;
        if (field == 4)
            desired_filter_.type = static_cast<uint8_t>(value);
        if (field == 5)
            desired_filter_.topology = static_cast<uint8_t>(value);
        if (field == 6)
            desired_filter_.slope = static_cast<uint8_t>(value);
        if (field == 7)
            desired_filter_.drive = value / 1000.f;
        operation_ = field < 2 || field >= 4 ? WaveX::Protocol::INST_EDIT_FILTER_SETTINGS
                                             : WaveX::Protocol::INST_EDIT_AMP;
        outgoing_ = desired_filter_ != Filter::Of(state_) ||
                    std::memcmp(&desired_, &state_.sound, sizeof(Sound)) != 0;
        return true;
    }
    WaveX::Protocol::InstEditOpMessage Request(uint32_t id, uint8_t op) const {
        WaveX::Protocol::InstEditOpMessage r;
        r.request_id = id;
        r.revision = state_.revision;
        r.track = state_.track;
        r.op = op;
        r.sound = desired_;
        if (op == WaveX::Protocol::INST_EDIT_FILTER_SETTINGS) {
            r.filter_type = desired_filter_.type;
            r.filter_topology = desired_filter_.topology;
            r.filter_slope = desired_filter_.slope;
            r.filter_drive = desired_filter_.drive;
        }
        return r;
    }

   private:
    State state_{};
    Sound desired_{}, sent_{};
    Filter desired_filter_{}, sent_filter_{};
    uint32_t expected_ = 0, pending_ = 0;
    uint8_t operation_ = 0, pending_op_ = 0;
    bool valid_ = false, outgoing_ = false;
};
}  // namespace wavex_ui
