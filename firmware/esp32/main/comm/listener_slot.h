// WaveX comm listener slot
#pragma once

// The host test build defines ESP_PLATFORM but supplies its own FreeRTOS mock,
// which declares the semaphore API directly and has no semphr.h.
#include "freertos/FreeRTOS.h"
#if defined(ESP_PLATFORM) && !defined(WAVEX_TEST_BUILD)
#include "freertos/semphr.h"
#endif

namespace WaveX {
namespace Comm {

/**
 * @brief A `{callback, user_data}` pair that a UI page can safely deregister.
 *
 * Comm listeners are registered by UI pages (passing `this`) on the UI task and
 * invoked from the UART RX task. Two things go wrong if the pair is a plain
 * pair of globals, and both were live defects before this existed:
 *
 * 1. **Torn pair.** The two writes are not atomic together, so the invoking
 *    task can pick up a new callback with the previous `user_data` and call a
 *    page's handler with another page's `this`.
 * 2. **Use-after-free.** The invoker can pass the null check, be descheduled,
 *    and resume inside a page that `onExit()` has since destroyed.
 *
 * Both are closed by holding the mutex *across the invocation*: `set()` cannot
 * complete while a callback is running, so by the time a page's
 * `set(nullptr, nullptr)` returns, no handler is in flight and the page is safe
 * to destroy. That guarantee is the whole point - releasing the mutex before
 * calling, which one of the hand-rolled versions did explicitly "to avoid
 * deadlocks", throws it away and leaves exactly the race above.
 *
 * @warning A callback invoked from here must never take the LVGL port lock.
 * Pages register and deregister from `onEnter`/`onExit`, which run with that
 * lock already held, so a callback that reached for it would invert the
 * LVGL -> listener order this class relies on and deadlock. Comm callbacks
 * stage data behind an atomic flag and let the UI task draw; see
 * `input_dispatcher.cpp` for the same rule stated from the other side.
 *
 * The mutex is recursive so a callback that re-registers itself - legitimate,
 * and a silent hang if it were not - simply works.
 */
template <typename Fn>
class ListenerSlot {
   public:
    // Constructed at static-init time, which on ESP-IDF runs from the main task
    // with the scheduler up and the heap available.
    ListenerSlot() : mutex_(xSemaphoreCreateRecursiveMutex()) {}

    // Not copyable: the mutex and the registration are identity, not value.
    ListenerSlot(const ListenerSlot&) = delete;
    ListenerSlot& operator=(const ListenerSlot&) = delete;

    /** Register, or clear with `set(nullptr, nullptr)`. Blocks until any
     *  in-flight invocation of the previous callback has returned. */
    void set(Fn fn, void* user_data) {
        const bool locked = take();
        fn_ = fn;
        user_data_ = user_data;
        if (locked) {
            give();
        }
    }

    /** Call the registered callback, if any, with `user_data` appended as the
     *  final argument - the shape every WaveX comm callback already has. */
    template <typename... Args>
    void invoke(Args... args) {
        const bool locked = take();
        Fn fn = fn_;
        void* user_data = user_data_;
        if (fn) {
            fn(args..., user_data);
        }
        if (locked) {
            give();
        }
    }

    /** True if a callback is registered. Advisory only: it can go stale the
     *  moment it returns, so it is for logging, not for guarding a call. */
    bool registered() const { return fn_ != nullptr; }

   private:
    bool take() {
        if (!mutex_) {
            // Creation only fails out of heap at startup. Degrading to an
            // unlocked call keeps the link working rather than silently
            // dropping every message; it is strictly no worse than the
            // hand-rolled slots this replaced.
            return false;
        }
        return xSemaphoreTakeRecursive(mutex_, portMAX_DELAY) == pdTRUE;
    }
    void give() { xSemaphoreGiveRecursive(mutex_); }

    SemaphoreHandle_t mutex_ = nullptr;
    Fn fn_ = nullptr;
    void* user_data_ = nullptr;
};

}  // namespace Comm
}  // namespace WaveX
