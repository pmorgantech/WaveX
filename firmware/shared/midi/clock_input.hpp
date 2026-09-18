#pragma once
#include "spi_protocol/protocol.h"

#include <cstdint>
namespace WaveX::Midi {
// One instance per physical input, owned by that input's byte consumer.
// SPP assembly is independent of the note parser; real-time bytes never
// disturb either parser, including when they interrupt F2 or SysEx.
class ClockInput {
   public:
    explicit ClockInput(uint8_t source = 0) : source_(source) {}
    void Reset() {
        count_ = 0;
        have_tick_ = false;
    }
    bool Feed(uint8_t byte, uint32_t now_us, Protocol::MidiClockEventMessage& out) {
        using namespace Protocol;
        uint8_t event;
        uint16_t spp = 0;
        if (byte >= 0xf8) {
            switch (byte) {
                case 0xf8:
                    event = MIDI_CLK_TICK;
                    break;
                case 0xfa:
                    event = MIDI_CLK_START;
                    have_tick_ = false;
                    break;
                case 0xfb:
                    event = MIDI_CLK_CONTINUE;
                    have_tick_ = false;
                    break;
                case 0xfc:
                    event = MIDI_CLK_STOP;
                    break;
                default:
                    return false;
            }
        } else if (byte & 0x80) {
            count_ = byte == 0xf2 ? 1 : 0;
            return false;
        } else if (count_ == 1) {
            low_ = byte;
            count_ = 2;
            return false;
        } else if (count_ == 2) {
            spp = static_cast<uint16_t>(low_ | (uint16_t(byte) << 7));
            count_ = 0;
            event = MIDI_CLK_SPP;
        } else
            return false;
        uint32_t delta = 0;
        if (event == MIDI_CLK_TICK) {
            ++sequence_;
            if (!have_tick_ || now_us != previous_tick_) {
                const uint16_t clocks = static_cast<uint16_t>(sequence_ - previous_sequence_);
                if (have_tick_ && clocks)
                    delta = (now_us - previous_tick_) / clocks;
                previous_tick_ = now_us;
                previous_sequence_ = sequence_;
                have_tick_ = true;
            }
            // Equal USB timestamps are a batch, not an infinite tempo. The
            // next distinct timestamp measures the average period of the
            // preceding group, using its actual clock count.
        }
        out = {event, source_, sequence_, delta, spp};
        return true;
    }

   private:
    uint8_t source_ = 0, count_ = 0, low_ = 0;
    uint16_t sequence_ = 0, previous_sequence_ = 0;
    uint32_t previous_tick_ = 0;
    bool have_tick_ = false;
};
}  // namespace WaveX::Midi
