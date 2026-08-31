#pragma once

// HAL-free single-producer/single-consumer note-event queue. Note-off events
// get a coalescing overflow path so queue pressure may drop a trigger, but can
// never leave an already-sounding note permanently held.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace WaveX {
namespace AudioEngine {

template <typename Event, uint32_t Capacity>
class NoteEventQueue {
   public:
    static_assert(Capacity > 0, "NoteEventQueue capacity must be non-zero");
    static_assert(std::is_trivially_copyable<Event>::value,
                  "NoteEventQueue events cross an IRQ boundary by value");

    static constexpr uint32_t kMidiNoteCount = 128;
    static constexpr uint32_t kReleaseWordBits = 32;
    static constexpr uint32_t kReleaseWordCount = kMidiNoteCount / kReleaseWordBits;

    void Init() {
        __atomic_store_n(&write_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&read_, 0u, __ATOMIC_RELAXED);
        for (uint32_t& word: overflow_releases_) {
            __atomic_store_n(&word, 0u, __ATOMIC_RELAXED);
        }
    }

    // Main-loop producer.
    bool Push(const Event& event) {
        const uint32_t write = __atomic_load_n(&write_, __ATOMIC_RELAXED);
        const uint32_t read = __atomic_load_n(&read_, __ATOMIC_ACQUIRE);
        if (write - read >= Capacity) {
            return false;
        }
        events_[write % Capacity] = event;
        __atomic_store_n(&write_, write + 1u, __ATOMIC_RELEASE);
        return true;
    }

    // Main-loop producer. Returns false when the queue was full, even though
    // the release has been preserved in the coalescing bitmap.
    bool PushReleaseOrRemember(const Event& event) {
        if (Push(event)) {
            return true;
        }
        const uint32_t note = event.note;
        if (note < kMidiNoteCount) {
            const uint32_t word = note / kReleaseWordBits;
            const uint32_t bit = 1u << (note % kReleaseWordBits);
            __atomic_fetch_or(&overflow_releases_[word], bit, __ATOMIC_RELEASE);
        }
        return false;
    }

    // Audio-callback consumer.
    bool Pop(Event& event) {
        const uint32_t read = __atomic_load_n(&read_, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&write_, __ATOMIC_ACQUIRE);
        if (read == write) {
            return false;
        }
        event = events_[read % Capacity];
        __atomic_store_n(&read_, read + 1u, __ATOMIC_RELEASE);
        return true;
    }

    // Audio-callback consumer. Each bit is one MIDI note that must receive a
    // release after the ordinary queue has been drained.
    uint32_t TakeOverflowReleaseWord(uint32_t word) {
        if (word >= kReleaseWordCount) {
            return 0;
        }
        return __atomic_exchange_n(&overflow_releases_[word], 0u, __ATOMIC_ACQ_REL);
    }

   private:
    Event events_[Capacity] = {};
    uint32_t write_ = 0;
    uint32_t read_ = 0;
    uint32_t overflow_releases_[kReleaseWordCount] = {};
};

}  // namespace AudioEngine
}  // namespace WaveX
