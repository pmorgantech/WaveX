#pragma once
#include "task_mutex.h"

#include <array>
#include <cstddef>
#include <type_traits>

namespace WaveX::Comm {
// Whole values cross the task boundary. The lock is released before consumers
// call page, link or LVGL APIs. Overflow is explicit and must fail closed.
template <typename T, size_t Capacity>
class ValueQueue {
    static_assert(std::is_trivially_copyable<T>::value, "Queue values own no pointers");
    static_assert(Capacity > 0, "Nonempty queue");

   public:
    bool Push(const T& value) {
        TaskLock lock(mutex_);
        if (!lock)
            return false;
        if (size_ == Capacity) {
            overflow_ = true;
            return false;
        }
        values_[(head_ + size_) % Capacity] = value;
        ++size_;
        return true;
    }
    bool Pop(T& value) {
        TaskLock lock(mutex_);
        if (!lock || !size_)
            return false;
        value = values_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }
    bool TakeOverflow() {
        TaskLock lock(mutex_);
        if (!lock)
            return true;
        const bool result = overflow_;
        overflow_ = false;
        return result;
    }
    void Clear() {
        TaskLock lock(mutex_);
        if (lock) {
            size_ = head_ = 0;
            overflow_ = false;
        }
    }

   private:
    TaskMutex mutex_;
    std::array<T, Capacity> values_{};
    size_t head_ = 0, size_ = 0;
    bool overflow_ = false;
};
}  // namespace WaveX::Comm
