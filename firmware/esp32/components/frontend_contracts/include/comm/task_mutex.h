#pragma once

#if defined(ESP_PLATFORM) && !defined(WAVEX_TEST_BUILD)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <mutex>
#endif

namespace WaveX::Comm {
// Ordinary-task ownership only. Never use from an ISR or hold across LVGL calls.
class TaskMutex {
   public:
    TaskMutex() = default;
    TaskMutex(const TaskMutex&) = delete;
    TaskMutex& operator=(const TaskMutex&) = delete;
#if defined(ESP_PLATFORM) && !defined(WAVEX_TEST_BUILD)
    ~TaskMutex() {
        if (mutex_)
            vSemaphoreDelete(mutex_);
    }
    bool Lock() { return mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE; }
    void Unlock() { xSemaphoreGive(mutex_); }

   private:
    SemaphoreHandle_t mutex_ = xSemaphoreCreateMutex();
#else
    bool Lock() {
        mutex_.lock();
        return true;
    }
    void Unlock() { mutex_.unlock(); }

   private:
    std::mutex mutex_;
#endif
};
class TaskLock {
   public:
    explicit TaskLock(TaskMutex& mutex) : mutex_(mutex), locked_(mutex.Lock()) {}
    ~TaskLock() {
        if (locked_)
            mutex_.Unlock();
    }
    explicit operator bool() const { return locked_; }
    TaskLock(const TaskLock&) = delete;
    TaskLock& operator=(const TaskLock&) = delete;

   private:
    TaskMutex& mutex_;
    bool locked_;
};
}  // namespace WaveX::Comm
