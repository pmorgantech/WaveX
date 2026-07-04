#pragma once

// Serial-MIDI byte-stream parser (roadmap Phase 1 item 8, "DIN + USB MIDI
// in"). HAL-free and header-only: consumes one raw byte at a time from any
// transport (ESP32 DIN UART today; a USB-MIDI event packet already carries
// framed 3-byte messages, but its payload bytes can be replayed through
// this parser too) and emits complete channel-voice events. Host-testable
// with no platform dependency - see tests/midi/midi_stream_parser_test.cpp.
//
// Implements the parts of the MIDI 1.0 byte-level grammar that matter for
// staying in sync on a real cable:
//  - running status (a status byte persists across consecutive messages of
//    the same type; data bytes arriving with no status are discarded)
//  - real-time bytes (0xF8-0xFF) may interleave *anywhere*, including
//    mid-message and inside SysEx, without disturbing parser state
//  - SysEx (0xF0..0xF7) is skipped, not buffered
//  - system-common messages (0xF1-0xF7) cancel running status per the spec
//    and their data bytes are consumed so the stream stays aligned
//  - every channel-voice message length is tracked (program change and
//    channel pressure are 2 bytes, the rest 3) so unhandled message types
//    keep the parser aligned instead of desyncing it
//  - NoteOn with velocity 0 is normalized to NoteOff (universal MIDI
//    convention, required for running-status note streams)

#include <cstdint>

namespace WaveX {
namespace Midi {

enum class EventType : uint8_t {
    NoteOn,         // data1 = note, data2 = velocity (always > 0 here)
    NoteOff,        // data1 = note, data2 = release velocity (0 when
                    // normalized from a velocity-0 NoteOn)
    ControlChange,  // data1 = controller, data2 = value
};

struct Event {
    EventType type = EventType::NoteOff;
    uint8_t channel = 0;  // 0-15
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

class StreamParser {
   public:
    // Feeds one raw byte. Returns true when `out` holds a complete event.
    // Bytes that complete unreported message types (pitch bend, aftertouch,
    // program change...) return false but still advance parser state.
    bool Feed(uint8_t byte, Event& out) {
        // Real-time bytes are transparent: valid anywhere, never touch
        // running status, SysEx state, or an in-progress message.
        if (byte >= 0xF8) {
            return false;
        }

        if (byte >= 0x80) {  // status byte
            if (byte == 0xF0) {
                in_sysex_ = true;
                status_ = 0;  // system messages cancel running status
                return false;
            }
            if (byte == 0xF7) {  // EOX - also terminates a dangling SysEx
                in_sysex_ = false;
                status_ = 0;
                return false;
            }
            in_sysex_ = false;  // a status byte implicitly ends SysEx
            if (byte >= 0xF1) {
                // System common: cancels running status; consume any data
                // bytes so the stream stays aligned (F1/F3: 1, F2: 2).
                status_ = 0;
                pending_system_data_ = (byte == 0xF2) ? 2 : (byte == 0xF1 || byte == 0xF3) ? 1 : 0;
                return false;
            }
            // Channel voice status: becomes the running status.
            status_ = byte;
            data_count_ = 0;
            return false;
        }

        // --- data byte (< 0x80) ---
        if (in_sysex_) {
            return false;  // SysEx payload: skipped, not buffered
        }
        if (pending_system_data_ > 0) {
            --pending_system_data_;
            return false;
        }
        if (status_ == 0) {
            return false;  // orphan data byte, no running status to apply
        }

        data_[data_count_++] = byte;
        const uint8_t needed = MessageDataLength(status_);
        if (data_count_ < needed) {
            return false;
        }
        data_count_ = 0;  // message complete; running status persists

        const uint8_t kind = status_ & 0xF0;
        const uint8_t channel = status_ & 0x0F;
        switch (kind) {
            case 0x90:  // NoteOn (velocity 0 => NoteOff)
                out.type = (data_[1] == 0) ? EventType::NoteOff : EventType::NoteOn;
                out.channel = channel;
                out.data1 = data_[0];
                out.data2 = data_[1];
                return true;
            case 0x80:  // NoteOff
                out.type = EventType::NoteOff;
                out.channel = channel;
                out.data1 = data_[0];
                out.data2 = data_[1];
                return true;
            case 0xB0:  // ControlChange
                out.type = EventType::ControlChange;
                out.channel = channel;
                out.data1 = data_[0];
                out.data2 = data_[1];
                return true;
            default:  // parsed for alignment, not reported
                return false;
        }
    }

    void Reset() {
        status_ = 0;
        data_count_ = 0;
        pending_system_data_ = 0;
        in_sysex_ = false;
    }

   private:
    static uint8_t MessageDataLength(uint8_t status) {
        const uint8_t kind = status & 0xF0;
        // Program change (0xC0) and channel pressure (0xD0) carry one data
        // byte; every other channel-voice message carries two.
        return (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
    }

    uint8_t status_ = 0;      // current running status, 0 = none
    uint8_t data_[2] = {};    // accumulated data bytes
    uint8_t data_count_ = 0;  // how many of data_[] are filled
    uint8_t pending_system_data_ = 0;
    bool in_sysex_ = false;
};

}  // namespace Midi
}  // namespace WaveX
