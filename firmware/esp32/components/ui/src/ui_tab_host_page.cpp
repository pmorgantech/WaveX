// WaveX tabbed host page - shows one child UIPage at a time
#include "ui/ui_tab_host_page.h"

#include <esp_log.h>

#include "ui/ui_navigator.h"
#include "ui/ui_tab_group.h"

namespace wavex_ui {

namespace {
static const char* TAG = "UI_TAB_HOST";
}  // namespace

void UITabHostPage::addTab(const char* title, std::shared_ptr<UIPage> page) {
    if (!page) {
        return;
    }
    Tab t;
    t.title = title ? title : "";
    t.page = std::move(page);
    tabs_.push_back(std::move(t));
}

void UITabHostPage::onEnter(lv_obj_t* parent) {
    lv_obj_clean(parent);
    active_ = -1;

    root_ = tabGroupCreate(parent);
    tabview_ = root_;

    for (auto& t: tabs_) {
        t.body = tabGroupAddTab(tabview_, t.title.c_str());
    }

    lv_obj_add_event_cb(tabview_, &UITabHostPage::tabChangedCb, LV_EVENT_VALUE_CHANGED, this);

    if (!tabs_.empty()) {
        activate(0);
    }
}

void UITabHostPage::onExit() {
    // Exit the live child first: it must tear down its own timers and widgets
    // before the tab bodies holding them are deleted, or an lv_timer left
    // running would fire against freed objects.
    if (UIPage* p = activePage()) {
        p->onExit();
    }
    active_ = -1;

    if (root_) {
        lv_obj_del(root_);
        root_ = nullptr;
        tabview_ = nullptr;
    }
    for (auto& t: tabs_) {
        t.body = nullptr;
    }
}

void UITabHostPage::tabChangedCb(lv_event_t* e) {
    auto* self = static_cast<UITabHostPage*>(lv_event_get_user_data(e));
    if (!self || !self->tabview_) {
        return;
    }
    self->activate(static_cast<int>(lv_tabview_get_tab_active(self->tabview_)));
}

void UITabHostPage::activate(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size()) || index == active_) {
        return;
    }
    // Tear the outgoing child down before building the incoming one. Both at
    // once would briefly double this page's LVGL memory, and a page that polls
    // would keep polling from behind a tab nobody is looking at.
    if (UIPage* prev = activePage()) {
        prev->onExit();
    }

    active_ = index;
    Tab& t = tabs_[index];
    if (t.page && t.body) {
        t.page->onEnter(t.body);
    }

    // The child owns the softkey row while it is visible, so the bar has to be
    // rebuilt from the new child rather than left showing the old one's keys.
    UINavigator::instance().refreshSoftkeys();
    ESP_LOGI(TAG, "%s -> %s", name_.c_str(), t.title.c_str());
}

UIPage* UITabHostPage::activePage() const {
    if (active_ < 0 || active_ >= static_cast<int>(tabs_.size())) {
        return nullptr;
    }
    return tabs_[static_cast<size_t>(active_)].page.get();
}

void UITabHostPage::onInput(const InputEvent& evt) {
    if (UIPage* p = activePage()) {
        p->onInput(evt);
    }
}

std::array<Softkey, NUM_SOFTKEYS> UITabHostPage::getSoftkeys() {
    if (UIPage* p = activePage()) {
        return p->getSoftkeys();
    }
    return {};
}

std::array<Softkey, NUM_SOFTKEYS> UITabHostPage::getShiftedSoftkeys() {
    if (UIPage* p = activePage()) {
        return p->getShiftedSoftkeys();
    }
    return {};
}

}  // namespace wavex_ui
