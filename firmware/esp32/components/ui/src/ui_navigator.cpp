// WaveX UI Navigation Manager Implementation
#include "ui/ui_navigator.h"

#include <esp_log.h>

#include "../styles/ui_theme.h"
#include "esp_lvgl_port.h"
#include "ui/ui_status_strip.h"

// LVGL locking macros
#define LV_LOCK() lvgl_port_lock(portMAX_DELAY)
#define LV_UNLOCK() lvgl_port_unlock()

// LVGL Lock Usage Guidelines:
//
// The port lock is a recursive mutex, so taking it again on a path that
// already holds it is free. Prefer taking it to reasoning about whether some
// caller already did.
//
// - UI task: InputDispatcher::processAll() locks around each event, so every
//   UIPage::onInput handler and anything it calls already runs locked.
// - UINavigator::push/pop: take the lock; they are also called outside dispatch.
// - UIPage::onEnter/onExit: NO lock - called from push/pop, which hold it.
// - lv_timer and lv_obj event callbacks: NO lock - LVGL context already.
// - UART RX task (comm callbacks): must NOT touch widgets at all, with or
//   without the lock. Stage the data behind an atomic flag and let the owning
//   page apply it from its lv_timer or processDeferredUpdates_(). Note that
//   lv_async_call is not an escape hatch: it links an lv_timer itself, so
//   calling it off-context races the list it is deferring onto.

static const char* TAG = "UI_NAVIGATOR";

namespace wavex_ui {

void UINavigator::push(std::shared_ptr<UIPage> page) {
    if (!page) {
        ESP_LOGE(TAG, "Cannot push null page");
        return;
    }

    // Initialize screen and static regions if first page
    if (!screen_) {
        LV_LOCK();
        screen_ = lv_obj_create(nullptr);
        lv_scr_load(screen_);
        ESP_LOGI(TAG, "Created navigation screen");

        lv_obj_set_size(screen_, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(screen_, UI_COLOR_BACKGROUND, LV_PART_MAIN);
        lv_obj_set_style_border_width(screen_, 0, LV_PART_MAIN);

        header_ = lv_obj_create(screen_);
        lv_obj_set_size(header_, lv_pct(100), UI_HEADER_HEIGHT);
        lv_obj_set_style_bg_color(header_, UI_COLOR_HEADER, LV_PART_MAIN);
        lv_obj_set_style_border_width(header_, 0, LV_PART_MAIN);
        lv_obj_align(header_, LV_ALIGN_TOP_MID, 0, 0);

        title_label_ = lv_label_create(header_);
        lv_obj_set_style_text_color(title_label_, UI_COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(title_label_, UI_FONT_HEADER, LV_PART_MAIN);
        lv_obj_center(title_label_);

        // Output meters and engine CPU live in the header so they are visible
        // from every page, not just diagnostics.
        statusStripCreate(header_);
        buildShiftChip();

        // Content area fills between header and softkeys (we create softkeys later)
        content_ = lv_obj_create(screen_);
        lv_obj_set_size(
            content_, lv_pct(100), lv_pct(100));  // temporary; corrected after softkey create
        lv_obj_set_style_bg_color(content_, UI_COLOR_CONTENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(content_, 0, LV_PART_MAIN);
        lv_obj_align(content_, LV_ALIGN_TOP_LEFT, 0, UI_HEADER_HEIGHT);
        LV_UNLOCK();
    }

    enter(std::move(page), true);
}

void UINavigator::enter(std::shared_ptr<UIPage> page, bool exit_current) {
    if (exit_current && !stack_.empty()) {
        auto current = stack_.top();
        ESP_LOGI(TAG, "Exiting page: %s", current->name());
        LV_LOCK();
        current->onExit();
        LV_UNLOCK();
    }

    stack_.push(page);
    active_ = page;
    // Shift does not survive navigation - the next page's alternate row is a
    // different set of actions, and arriving already shifted is surprising.
    shifted_ = false;

    LV_LOCK();
    lv_obj_clean(content_);
    lv_label_set_text(title_label_, page->name());
    LV_UNLOCK();

    ESP_LOGI(TAG, "Entering page: %s", page->name());
    LV_LOCK();
    page->onEnter(content_);
    LV_UNLOCK();

    LV_LOCK();
    softkeyBar_.create(screen_);
    softkeyBar_.setSoftkeys(page->getSoftkeys());
    refreshShiftChip();
    layoutContent();
    LV_UNLOCK();

    ESP_LOGI(TAG, "Navigation stack depth: %zu", stack_.size());
}

void UINavigator::layoutContent() {
    const int32_t total_h = lv_obj_get_height(lv_screen_active());
    const int32_t content_h = total_h - UI_HEADER_HEIGHT - UI_HOTKEY_HEIGHT;
    lv_obj_set_size(content_, lv_pct(100), content_h > 0 ? content_h : 0);
    lv_obj_align(content_, LV_ALIGN_TOP_LEFT, 0, UI_HEADER_HEIGHT);
}

void UINavigator::pop() {
    if (stack_.size() <= 1) {
        ESP_LOGW(TAG, "Cannot pop root page");
        return;
    }

    if (!stack_.empty()) {
        auto current = stack_.top();
        ESP_LOGI(TAG, "Exiting page: %s", current->name());
        LV_LOCK();
        current->onExit();
        LV_UNLOCK();
        stack_.pop();
    }

    if (!stack_.empty()) {
        auto prev = stack_.top();
        active_ = prev;
        if (stack_.size() == 1) {
            root_group_ = RootGroup::None;
        }

        LV_LOCK();
        lv_obj_clean(content_);
        lv_label_set_text(title_label_, prev->name());
        LV_UNLOCK();

        ESP_LOGI(TAG, "Returning to page: %s", prev->name());
        LV_LOCK();
        prev->onEnter(content_);
        LV_UNLOCK();

        LV_LOCK();
        softkeyBar_.create(screen_);
        shifted_ = false;  // see push(): Shift does not survive navigation
        softkeyBar_.setSoftkeys(prev->getSoftkeys());
        refreshShiftChip();
        layoutContent();
        LV_UNLOCK();
    }

    ESP_LOGI(TAG, "Navigation stack depth: %zu", stack_.size());
}

void UINavigator::unwindToRoot() {
    if (stack_.size() <= 1) {
        return;
    }
    auto current = stack_.top();
    ESP_LOGI(TAG, "Exiting page: %s", current->name());
    LV_LOCK();
    current->onExit();
    LV_UNLOCK();
    // Every page below the top was exited when its child was pushed; there
    // is nothing to tear down, only references to drop.
    while (stack_.size() > 1) {
        stack_.pop();
    }
    root_group_ = RootGroup::None;
}

void UINavigator::popToRoot() {
    if (stack_.size() <= 1) {
        return;
    }
    unwindToRoot();
    // Re-enter the root as pop() would.
    auto root = stack_.top();
    stack_.pop();
    enter(root, false);
}

void UINavigator::setRootGroupFactory(RootGroup group, PageFactory factory) {
    const auto i = static_cast<size_t>(group);
    if (i < static_cast<size_t>(RootGroup::Count)) {
        factories_[i] = std::move(factory);
    }
}

bool UINavigator::hasRootGroup(RootGroup group) const {
    const auto i = static_cast<size_t>(group);
    return i < static_cast<size_t>(RootGroup::Count) && static_cast<bool>(factories_[i]);
}

bool UINavigator::jumpToRoot(RootGroup group) {
    if (!hasRootGroup(group)) {
        // Track and Mixer have keys before they have pages. Refusing here,
        // loudly, beats leaving the user on a page that is not the one the
        // key is labelled with.
        ESP_LOGW(TAG, "No page for root group %s yet", rootGroupName(group));
        return false;
    }
    if (stack_.empty()) {
        ESP_LOGE(TAG, "jumpToRoot before the main menu exists");
        return false;
    }
    auto page = factories_[static_cast<size_t>(group)]();
    if (!page) {
        ESP_LOGE(TAG, "Root group %s produced no page", rootGroupName(group));
        return false;
    }
    ESP_LOGI(TAG, "Jump to %s", rootGroupName(group));
    // At the main menu the root is live and enter() exits it; deeper, the
    // unwind has already exited the top and the root stays exited.
    const bool at_root = stack_.size() == 1;
    unwindToRoot();
    enter(std::move(page), at_root);
    root_group_ = group;
    return true;
}

void UINavigator::refreshSoftkeys() {
    if (!screen_ || !active_) {
        return;
    }

    // Rebuilds widgets, so LVGL context only: an lv_obj/lv_timer callback, or
    // the UI task, which holds the port lock across input dispatch. Callers
    // reached from the UART task must queue instead (see UISampleBrowser).
    const bool shifted = shifted_ && activePageHasShiftedKeys();
    softkeyBar_.setSoftkeys(shifted ? active_->getShiftedSoftkeys() : active_->getSoftkeys(),
                            shifted);
    refreshShiftChip();
}

bool UINavigator::activePageHasShiftedKeys() const {
    if (!active_) {
        return false;
    }
    // A page opts in by returning at least one labelled key. Anything else is
    // "no alternates", and Shift then leaves the row alone instead of blanking
    // it - pressing Shift on a page that does not use it should do nothing,
    // not strand the user with six empty buttons.
    for (const auto& k: active_->getShiftedSoftkeys()) {
        if (!k.label.empty()) {
            return true;
        }
    }
    return false;
}

void UINavigator::setShift(bool on) {
    if (shifted_ == on) {
        return;
    }
    shifted_ = on;
    refreshSoftkeys();
}

void UINavigator::toggleShift() {
    setShift(!shifted_);
}

void UINavigator::notifySoftkeyUsed() {
    // Sticky: one shifted action, then back to the normal row. A plain toggle
    // gets left on and the next press does the wrong thing.
    if (shifted_) {
        setShift(false);
    }
}

void UINavigator::buildShiftChip() {
    if (!header_) {
        return;
    }
    // Left of the header. The title is centred and the meters are anchored
    // right, so this corner is the only free space in the chrome.
    shift_chip_ = lv_btn_create(header_);
    lv_obj_set_size(shift_chip_, 104, 44);
    lv_obj_set_pos(shift_chip_, 12, 16);
    lv_obj_set_style_radius(shift_chip_, 4, 0);
    lv_obj_set_style_border_width(shift_chip_, 1, 0);

    shift_label_ = lv_label_create(shift_chip_);
    lv_label_set_text(shift_label_, "SHIFT");
    lv_obj_set_style_text_font(shift_label_, &lv_font_montserrat_18, 0);
    lv_obj_center(shift_label_);

    lv_obj_add_event_cb(shift_chip_, shiftChipEventCb, LV_EVENT_CLICKED, this);
    refreshShiftChip();
}

void UINavigator::refreshShiftChip() {
    if (!shift_chip_ || !lv_obj_is_valid(shift_chip_)) {
        return;
    }
    const bool available = activePageHasShiftedKeys();
    const bool on = shifted_ && available;

    lv_obj_set_style_bg_color(
        shift_chip_, on ? UI_COLOR_BUTTON_SHIFTED : UI_COLOR_BUTTON_DISABLED, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        shift_chip_, on ? UI_COLOR_BUTTON_SHIFTED : UI_COLOR_BORDER, LV_PART_MAIN);
    // Dimmed rather than hidden on pages with no alternates: a control that
    // disappears and reappears as you navigate is harder to learn than one
    // that is always there and sometimes inert.
    lv_obj_set_style_text_color(
        shift_label_, available ? UI_COLOR_TEXT : UI_COLOR_TEXT_DISABLED, LV_PART_MAIN);
    lv_obj_set_style_opa(shift_chip_, available ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
}

void UINavigator::shiftChipEventCb(lv_event_t* e) {
    auto* self = static_cast<UINavigator*>(lv_event_get_user_data(e));
    if (self && self->activePageHasShiftedKeys()) {
        self->toggleShift();
    }
}

}  // namespace wavex_ui
