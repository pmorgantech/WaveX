#include "ui/ui_api.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_demo.h"

namespace wavex_ui {

void ui_init_demo(lv_obj_t* root) {
    // Create patch list page by default
    createPatchListPage(root);
}

void ui_set_active_context(std::shared_ptr<UIContext> ctx) {
    InputDispatcher::instance().setActiveContext(std::move(ctx));
}

} // namespace wavex_ui


