#pragma once
#include "audio/live_note_id.hpp"
#include "audio/note_event_queue.hpp"

namespace WaveX::AudioEngine {
struct LiveNoteEvent {
    LiveNoteId id;
    uint32_t sequence = 0;
    uint16_t tracks = 0;   // destinations at note-on, never re-route note-off
    uint8_t velocity = 0;  // zero is release
};

// Foreground owns press/release serials; callback owns rendering. Overflow
// releases retain an exact FIFO watermark, never an unscoped pitch bitmap.
template <uint32_t Capacity = 64>
class LiveNoteQueue {
   public:
    void Init() {
        queue_.Init();
        for (auto& source: keys_)
            for (auto& key: source)
                key = {};
        sequence_ = refused_ = 0;
    }
    bool Press(uint8_t source, uint8_t note, uint8_t velocity, uint16_t tracks) {
        if (source >= 32 || note >= 128 || !velocity || velocity > 127)
            return false;
        auto& key = keys_[source][note];
        // Never alias an ancient held key after serial wrap. Reinitialization
        // requires the existing stopped-engine/session initialization boundary.
        if (key.pressed == UINT32_MAX) {
            ++refused_;
            return false;
        }
        const auto serial = ++key.pressed;
        const auto sequence = __atomic_add_fetch(&sequence_, 1u, __ATOMIC_RELEASE);
        if (queue_.Push({{serial, source, note}, sequence, tracks, velocity}))
            return true;
        ++refused_;
        return false;
    }
    bool Release(uint8_t source, uint8_t note) {
        if (source >= 32 || note >= 128)
            return false;
        auto& key = keys_[source][note];
        if (key.released == key.pressed)
            return true;  // unmatched off does not consume a future press
        const auto serial = ++key.released;
        if (queue_.Push({{serial, source, note}, 0, 0, 0}))
            return true;
        __atomic_store_n(&key.overflow, serial, __ATOMIC_RELEASE);
        return false;
    }
    bool Peek(LiveNoteEvent& event) const { return queue_.Peek(event); }
    bool Pop(LiveNoteEvent& event) { return queue_.Pop(event); }
    uint32_t LastSequence() const { return __atomic_load_n(&sequence_, __ATOMIC_ACQUIRE); }
    bool OverflowReleased(LiveNoteId id) const {
        return id.Valid() &&
               id.serial <= __atomic_load_n(&keys_[id.source][id.note].overflow, __ATOMIC_ACQUIRE);
    }
    uint32_t Refused() const { return refused_; }  // foreground diagnostic

   private:
    struct Key {
        uint32_t pressed = 0, released = 0, overflow = 0;
    };
    Key keys_[32][128]{};
    NoteEventQueue<LiveNoteEvent, Capacity> queue_;
    uint32_t sequence_ = 0, refused_ = 0;
};
}  // namespace WaveX::AudioEngine
