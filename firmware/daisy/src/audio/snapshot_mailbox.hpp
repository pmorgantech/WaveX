#pragma once

// HAL-free latest-value mailbox for one producer and one consumer split across
// foreground and IRQ context. Either side may be the producer. This is the
// standard triple-buffer ownership exchange: producer and consumer each own a
// private slot, and atomically swap it with the middle slot. They can therefore
// never read and write the same object concurrently.

#include <cstdint>
#include <type_traits>

namespace WaveX {
namespace AudioEngine {

template <typename T>
class SnapshotMailbox {
   public:
    static_assert(std::is_trivially_copyable<T>::value,
                  "SnapshotMailbox publishes state by bounded value copy");

    void Init(const T& initial) {
        slots_[0] = initial;
        slots_[1] = initial;
        slots_[2] = initial;
        producer_back_ = 2;
        consumer_front_ = 0;
        __atomic_store_n(&middle_state_, 1u, __ATOMIC_RELAXED);
    }

    // Single producer. Repeated publication replaces the pending middle value
    // while preserving the producer's private ownership of its next back slot.
    void Publish(const T& value) {
        slots_[producer_back_] = value;
        const uint32_t previous_middle =
            __atomic_exchange_n(&middle_state_, producer_back_ | kDirtyBit, __ATOMIC_ACQ_REL);
        producer_back_ = previous_middle & kIndexMask;
    }

    // Single consumer. The exchange clears dirty and makes the old front the
    // new middle before copying from the newly acquired private front slot.
    bool ConsumeLatest(T& value) {
        if ((__atomic_load_n(&middle_state_, __ATOMIC_ACQUIRE) & kDirtyBit) == 0u) {
            return false;
        }
        const uint32_t previous_middle =
            __atomic_exchange_n(&middle_state_, consumer_front_, __ATOMIC_ACQ_REL);
        consumer_front_ = previous_middle & kIndexMask;
        value = slots_[consumer_front_];
        return true;
    }

   private:
    static constexpr uint32_t kIndexMask = 0x3u;
    static constexpr uint32_t kDirtyBit = 0x4u;

    T slots_[3] = {};
    uint32_t middle_state_ = 1;
    uint32_t producer_back_ = 2;
    uint32_t consumer_front_ = 0;
};

}  // namespace AudioEngine
}  // namespace WaveX
