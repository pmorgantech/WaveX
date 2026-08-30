// WaveX Input Dispatcher: FreeRTOS queue + context dispatch
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ui/input_event.h"
#include "ui/ui_context.h"

#include <atomic>
#include <memory>

namespace wavex_ui {

class InputDispatcher {
   public:
    static InputDispatcher& instance();

    bool postFromISR(const InputEvent& evt, BaseType_t* hpTaskWoken);
    bool post(const InputEvent& evt, TickType_t ticksToWait = 0);
    void processAll();
    void setActiveContext(std::shared_ptr<UIContext> ctx);

    /// Events dropped because the queue was full. A non-zero value means input
    /// was lost - a keypress or detent the user made and the instrument never
    /// saw - so it belongs on the diagnostics page rather than nowhere, which
    /// is where it went before.
    uint32_t droppedEvents() const { return dropped_events_.load(std::memory_order_relaxed); }

   private:
    InputDispatcher();
    QueueHandle_t queue_;
    std::shared_ptr<UIContext> current_;
    // Producers are the encoder poll, the keypad task and (potentially) an
    // ISR, so this is incremented from more than one context.
    std::atomic<uint32_t> dropped_events_{0};
};

}  // namespace wavex_ui
