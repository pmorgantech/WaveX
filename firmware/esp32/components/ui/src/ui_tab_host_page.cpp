// WaveX tabbed host page - shows one child UIPage at a time
#include "ui/ui_tab_host_page.h"

#include <esp_log.h>
#include <strings.h>

#include "debug/console_command.h"
#include "ui/ui_navigator.h"
#include "ui/ui_tab_group.h"

#include <cstring>

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

// Debug harness: which tab is live plus each tab button's centre (so a host
// can TAP it through the real tab bar), then the live child's own state.
// "PAGE TAB <title>" switches tabs; anything else is the child's to answer.
size_t UITabHostPage::consoleState(char* out, size_t cap, size_t len) {
    using namespace WaveX::Debug;
    UIPage* p = activePage();
    len = AppendKvText(
        out, cap, len, "tab", p ? tabs_[static_cast<size_t>(active_)].title.c_str() : "-");
    lv_obj_t* bar = tabview_ ? lv_tabview_get_tab_bar(tabview_) : nullptr;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        char key[16];
        snprintf(key, sizeof(key), "tab%u", static_cast<unsigned>(i & 0xFF));
        len = AppendKvText(out, cap, len, key, tabs_[i].title.c_str());
        lv_obj_t* btn = bar && i < lv_obj_get_child_count(bar)
                            ? lv_obj_get_child(bar, static_cast<int32_t>(i))
                            : nullptr;
        if (btn) {
            lv_obj_update_layout(btn);
            lv_area_t a;
            lv_obj_get_coords(btn, &a);
            char xy[16], v[24];
            snprintf(xy, sizeof(xy), "tab%uxy", static_cast<unsigned>(i & 0xFF));
            snprintf(v,
                     sizeof(v),
                     "%ld,%ld",
                     static_cast<long>((a.x1 + a.x2) / 2),
                     static_cast<long>((a.y1 + a.y2) / 2));
            len = AppendKv(out, cap, len, xy, v);
        }
    }
    return p ? p->consoleState(out, cap, len) : len;
}

bool UITabHostPage::consoleCommand(const char* args, char* reply, size_t cap) {
    using namespace WaveX::Debug;
    char verb[16];
    const char* p = args;
    NextWord(&p, verb, sizeof(verb));
    if (strcmp(verb, "TAB") == 0) {
        p = detail::SkipSpaces(p);
        for (size_t i = 0; i < tabs_.size(); ++i) {
            if (strcasecmp(tabs_[i].title.c_str(), p) == 0) {
                // lv_tabview_set_active() moves the view but only a tab-bar
                // click raises VALUE_CHANGED, so activate the child here the
                // way tabChangedCb would. (A host wanting the full touch path
                // taps tab<i>xy instead.)
                if (tabview_) {
                    lv_tabview_set_active(tabview_, static_cast<uint32_t>(i), LV_ANIM_OFF);
                }
                activate(static_cast<int>(i));
                reply[0] = '\0';
                return true;
            }
        }
        snprintf(reply, cap, "notab");
        return false;
    }
    UIPage* child = activePage();
    if (child) {
        return child->consoleCommand(args, reply, cap);
    }
    snprintf(reply, cap, "unknown");
    return false;
}

std::array<Softkey, NUM_SOFTKEYS> UITabHostPage::getShiftedSoftkeys() {
    if (UIPage* p = activePage()) {
        return p->getShiftedSoftkeys();
    }
    return {};
}

}  // namespace wavex_ui
