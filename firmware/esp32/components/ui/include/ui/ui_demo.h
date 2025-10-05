#pragma once

#include <memory>
#include "lvgl.h"
#include "ui/ui_context.h"

namespace wavex_ui {

// Create contexts
std::shared_ptr<UIContext> createPatchListContext();
std::shared_ptr<UIContext> createParamEditContext();

// Create pages
lv_obj_t* createPatchListPage(lv_obj_t* parent);
lv_obj_t* createParamEditPage(lv_obj_t* parent);

} // namespace wavex_ui


