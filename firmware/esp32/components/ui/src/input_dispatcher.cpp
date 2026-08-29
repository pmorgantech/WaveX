#include "ui/input_dispatcher.h"

#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "ui/ui_navigator.h"
#include "ui/ui_softkey.h"

namespace wavex_ui {

static constexpr size_t kQueueLength = 64;

InputDispatcher& InputDispatcher::instance() {
    static InputDispatcher inst;
    return inst;
}

InputDispatcher::InputDispatcher() {
    queue_ = xQueueCreate(kQueueLength, sizeof(InputEvent));
}

bool InputDispatcher::postFromISR(const InputEvent& evt, BaseType_t* hpTaskWoken) {
    if (!queue_)
        return false;
    return xQueueSendFromISR(queue_, &evt, hpTaskWoken) == pdTRUE;
}

bool InputDispatcher::post(const InputEvent& evt, TickType_t ticksToWait) {
    if (!queue_)
        return false;
    return xQueueSend(queue_, &evt, ticksToWait) == pdTRUE;
}

void InputDispatcher::processAll() {
    if (!queue_)
        return;
    InputEvent evt;
    while (xQueueReceive(queue_, &evt, 0) == pdTRUE) {
        // Handlers build and restyle widgets, so dispatch runs under the LVGL
        // port lock; the LVGL task renders on the other core and an unlocked
        // handler corrupts the object tree.
        //
        // Taken per event rather than once around the whole drain: a backlog
        // (a fast encoder spin queued while a page was still building) would
        // otherwise hold the lock for every event in it back to back, stalling
        // the render task for as many frames as there are events. Per event the
        // hold is one handler long and the renderer interleaves. The extra
        // acquire/release is a few hundred cycles against a queue that carries
        // single-digit events per 32 ms pass.
        //
        // Lock order is LVGL -> UART: handlers send over the link
        // (inter_mcu_send_*), which takes s_uart_mutex briefly and with a
        // timeout. Nothing may take these in the other order - in particular
        // the UART RX task's callbacks must stay flag-only, never touching
        // LVGL, or this becomes a deadlock.
        lvgl_port_lock(portMAX_DELAY);

        // Shift is a global modifier, handled here rather than per page: every
        // screen gets it for free, and no page can accidentally swallow it by
        // consuming ButtonPress for something else. Never forwarded; pages see
        // modifier state, not the key.
        if (evt.source_id == BUTTON_SHIFT) {
            if (evt.type == InputType::ButtonPress) {
                UINavigator::instance().toggleShift();
            }
        } else if (current_) {
            current_->handleEvent(evt);
        }

        lvgl_port_unlock();
    }
}

void InputDispatcher::setActiveContext(std::shared_ptr<UIContext> ctx) {
    current_ = std::move(ctx);
}

}  // namespace wavex_ui
