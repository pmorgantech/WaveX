#include <memory>
#include <algorithm>
#include "lvgl.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_demo.h"

namespace wavex_ui {

static lv_obj_t* s_label_value = nullptr;
static int s_param_value = 50;

static void updateLabel() {
    if (!s_label_value) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Parameter = %d", s_param_value);
    lv_label_set_text(s_label_value, buf);
}

static void paramEditHandler(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            s_param_value = std::max(0, s_param_value - 1);
            updateLabel();
            break;
        case InputType::EncoderRight:
            s_param_value = std::min(127, s_param_value + 1);
            updateLabel();
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            InputDispatcher::instance().setActiveContext(createPatchListContext());
            break;
        default:
            break;
    }
}

std::shared_ptr<UIContext> createParamEditContext() {
    return std::make_shared<UIContext>("ParamEdit", paramEditHandler);
}

lv_obj_t* createParamEditPage(lv_obj_t* parent) {
    s_label_value = lv_label_create(parent);
    lv_obj_center(s_label_value);
    updateLabel();
    return s_label_value;
}

} // namespace wavex_ui


