#include "ui/ui_api.h"

#include <atomic>
namespace {
wavex_ui::UISharedContext context;
std::atomic<bool> content_changed{false};
}  // namespace
namespace wavex_ui {
void uiInitialize(const UISharedContext& value) {
    context = value;
}
const UISharedContext& uiContext() {
    return context;
}
bool takeUIContentChanged() {
    return content_changed.exchange(false, std::memory_order_relaxed);
}
}  // namespace wavex_ui
void wavex_ui_mark_content_changed() {
    content_changed.store(true, std::memory_order_relaxed);
}
