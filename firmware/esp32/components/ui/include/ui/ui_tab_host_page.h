// WaveX tabbed host page - shows one child UIPage at a time
#pragma once

#include <lvgl.h>

#include "ui_page.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace wavex_ui {

/**
 * @brief A page whose body is a tab bar over several child pages.
 *
 * Hosts existing `UIPage` implementations unchanged: each child keeps its own
 * `onEnter`/`onExit`, softkeys and input handling, and this class forwards the
 * page contract to whichever tab is selected. That matters because the pages
 * being grouped (the sample browser, editor, manager and recorder) are
 * substantial and working - converting them into tab-body builders would be a
 * large, risky rewrite for a navigation change.
 *
 * Children are entered LAZILY and exited when switched away from, so only the
 * visible tab holds LVGL objects or runs refresh timers. A page that polls the
 * backend therefore stops polling when its tab is hidden, which is the same
 * discipline the diagnostics page already applies by subscribing only while
 * open.
 *
 * Two consequences worth knowing when adding a tab:
 *
 * - A child gets the content area **less the 56 px tab bar**. Pages that were
 *   already vertically full will be tighter.
 * - A child's "Back" softkey pops the navigator, which exits the whole group
 *   rather than returning to a sibling tab. That is intended: the tab bar is
 *   how you move between siblings.
 */
class UITabHostPage : public UIPage {
   public:
    explicit UITabHostPage(const char* name) : name_(name) {}

    /// Adds a tab. Call before the page is pushed; order is bar order.
    void addTab(const char* title, std::shared_ptr<UIPage> page);

    const char* name() const override { return name_.c_str(); }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;

   private:
    struct Tab {
        std::string title;
        std::shared_ptr<UIPage> page;
        lv_obj_t* body = nullptr;
    };

    static void tabChangedCb(lv_event_t* e);
    void activate(int index);
    UIPage* activePage() const;

    std::string name_;
    std::vector<Tab> tabs_;
    lv_obj_t* tabview_ = nullptr;
    int active_ = -1;
};

}  // namespace wavex_ui
