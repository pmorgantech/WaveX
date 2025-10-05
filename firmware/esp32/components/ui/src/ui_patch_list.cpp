#include <memory>
#include <string>
#include "lvgl.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_demo.h"

namespace wavex_ui {

static lv_obj_t* s_list = nullptr;

static void patchListHandler(const InputEvent& evt) {
    switch (evt.type) {
        case InputType::EncoderLeft:
            lv_group_focus_prev(lv_group_get_default());
            break;
        case InputType::EncoderRight:
            lv_group_focus_next(lv_group_get_default());
            break;
        case InputType::ButtonPress:
        case InputType::EncoderClick:
            InputDispatcher::instance().setActiveContext(createParamEditContext());
            break;
        default:
            break;
    }
}

std::shared_ptr<UIContext> createPatchListContext() {
    return std::make_shared<UIContext>("PatchList", patchListHandler);
}

lv_obj_t* createPatchListPage(lv_obj_t* parent) {
    s_list = lv_list_create(parent);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    for (int i = 0; i < 10; ++i) {
        std::string label = std::string("Patch ") + std::to_string(i);
        lv_list_add_text(s_list, label.c_str());
    }
    if (lv_group_get_default()) {
        lv_group_add_obj(lv_group_get_default(), s_list);
    }
    return s_list;
}

} // namespace wavex_ui


