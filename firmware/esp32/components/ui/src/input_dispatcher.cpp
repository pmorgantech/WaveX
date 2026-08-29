#include "ui/input_dispatcher.h"

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
        // Shift is a global modifier, handled here rather than per page: every
        // screen gets it for free, and no page can accidentally swallow it by
        // consuming ButtonPress for something else.
        if (evt.source_id == BUTTON_SHIFT) {
            if (evt.type == InputType::ButtonPress) {
                UINavigator::instance().toggleShift();
            }
            continue;  // never forwarded; pages see modifier state, not the key
        }
        if (current_) {
            current_->handleEvent(evt);
        }
    }
}

void InputDispatcher::setActiveContext(std::shared_ptr<UIContext> ctx) {
    current_ = std::move(ctx);
}

}  // namespace wavex_ui
