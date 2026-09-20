#pragma once
#include <cstdint>

namespace WaveX::AudioEngine {
// Source 0..15 is a MIDI channel; 16..31 is an explicitly addressed Track.
// Serial is FIFO per (source, pitch), including presses refused by the queue.
// A render group ID is separate: stealing never changes key-release identity.
struct LiveNoteId {
    uint32_t serial = 0;
    uint8_t source = 0, note = 0;
    bool Valid() const { return serial && source < 32 && note < 128; }
    bool Matches(const LiveNoteId& other, bool through = false) const {
        return Valid() && other.Valid() && source == other.source && note == other.note &&
               (through ? serial <= other.serial : serial == other.serial);
    }
};
}  // namespace WaveX::AudioEngine
