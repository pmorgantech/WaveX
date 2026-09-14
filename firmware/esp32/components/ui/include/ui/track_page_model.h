#pragma once
#include "spi_protocol/protocol.h"

#include <cstring>
namespace wavex_ui {
class TrackMixModel {
   public:
    void Reset(uint8_t track) {
        state_ = {};
        state_.track = track;
        expected_ = 0;
        valid_ = ready_ = false;
    }
    void Expect(uint32_t id) { expected_ = id; }
    void Block() {
        ready_ = false;
        expected_ = 0;
    }
    bool Accept(const WaveX::Protocol::MixStateMessage& s) {
        if (!expected_ || s.request_id != expected_ || s.track != state_.track ||
            !WaveX::Protocol::IsValidMixState(s))
            return false;
        state_ = s;
        valid_ = ready_ = true;
        expected_ = 0;
        return true;
    }
    bool Valid() const { return valid_; }
    bool Ready() const { return ready_; }
    const WaveX::Protocol::MixStateMessage& State() const { return state_; }

   private:
    WaveX::Protocol::MixStateMessage state_{};
    uint32_t expected_ = 0;
    bool valid_ = false, ready_ = false;
};

class TrackPageModel {
   public:
    void Reset(uint8_t track) {
        state_ = {};
        state_.track = track;
        expected_ = 0;
        valid_ = false;
    }
    void Expect(uint32_t id) { expected_ = id; }
    bool Accept(const WaveX::Protocol::TrackStateMessage& s) {
        if (!expected_ || s.request_id != expected_ || s.track != state_.track || s.track >= 16 ||
            s.valid != 1 || s.loaded > 1 || s.busy > 1 || s.mode > 1 || s.program_change > 1 ||
            !WaveX::Protocol::TrackMidiInValid(s.midi_in) ||
            !std::memchr(s.name, 0, sizeof(s.name)))
            return false;
        state_ = s;
        valid_ = true;
        return true;
    }
    bool Ready() const { return valid_ && !state_.busy; }
    bool Valid() const { return valid_; }
    const WaveX::Protocol::TrackStateMessage& State() const { return state_; }
    static uint8_t StepMidi(uint8_t current, int delta) {
        int index = current == WaveX::Protocol::TRACK_MIDI_IN_OFF ? 17 : current;
        index += delta;
        index = index < 0 ? 0 : (index > 17 ? 17 : index);
        return index == 17 ? static_cast<uint8_t>(WaveX::Protocol::TRACK_MIDI_IN_OFF)
                           : static_cast<uint8_t>(index);
    }

   private:
    WaveX::Protocol::TrackStateMessage state_{};
    uint32_t expected_ = 0;
    bool valid_ = false;
};
}  // namespace wavex_ui
