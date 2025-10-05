// WaveX Input Dispatcher: FreeRTOS queue + context dispatch
#pragma once

#include <memory>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ui/input_event.h"
#include "ui/ui_context.h"

namespace wavex_ui {

class InputDispatcher {
public:
    static InputDispatcher& instance();

    bool postFromISR(const InputEvent& evt, BaseType_t* hpTaskWoken);
    bool post(const InputEvent& evt, TickType_t ticksToWait = 0);
    void processAll();
    void setActiveContext(std::shared_ptr<UIContext> ctx);

private:
    InputDispatcher();
    QueueHandle_t queue_;
    std::shared_ptr<UIContext> current_;
};

} // namespace wavex_ui


