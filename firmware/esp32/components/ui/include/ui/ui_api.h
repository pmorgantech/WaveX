#pragma once

#include "lvgl.h"
#include <memory>
#include "ui/ui_context.h"

namespace wavex_ui {

void ui_init_demo(lv_obj_t* root);
void ui_set_active_context(std::shared_ptr<UIContext> ctx);

}


