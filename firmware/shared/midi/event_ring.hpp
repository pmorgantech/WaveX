#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
namespace WaveX::Midi {
// Fixed SPSC handoff: producer never waits for consumer or touches its index.
template <typename T, uint32_t Capacity>
class EventRing {
   public:
    static_assert(Capacity && !(Capacity & (Capacity - 1)) && std::is_trivially_copyable<T>::value,
                  "fixed value messages only");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "IRQ handoff must be lock free");
    void Init() {
        read_.store(0);
        write_.store(0);
        dropped_.store(0);
    }
    bool Push(const T& value) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        values_[w % Capacity] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& value) {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;
        value = values_[r % Capacity];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    uint32_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

   private:
    std::array<T, Capacity> values_{};
    std::atomic<uint32_t> read_{0}, write_{0}, dropped_{0};
};
}  // namespace WaveX::Midi
