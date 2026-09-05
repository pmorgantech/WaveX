// WaveX UI Navigation Manager (Stack-Based)
#pragma once

#include "root_group.h"
#include "ui_page.h"
#include "ui_softkey_bar.h"

#include <functional>
#include <memory>
#include <stack>

namespace wavex_ui {

/// Stack-based navigation manager: push/pop UI pages, driving page lifecycle
/// (onEnter/onExit) and softkey bar updates automatically.
class UINavigator {
   public:
    static UINavigator& instance() {
        static UINavigator inst;
        return inst;
    }

    /// Takes ownership of `page`.
    void push(std::shared_ptr<UIPage> page);

    /// Does nothing if only one page remains (root page).
    void pop();

    /**
     * @brief Unwind to the main menu in one step.
     *
     * Only the top page is live (each page below it was exited when its
     * child was pushed), so this exits the top, drops the rest and re-enters
     * the root - the intermediate pages are never rebuilt, which pop() in a
     * loop would do. No-op at the root.
     */
    void popToRoot();

    /**
     * @brief Jump to a root group from anywhere: unwind to the main menu, then
     *        push the group's page (panel-controls.md §4.3).
     *
     * No state is preserved across a jump; that is what makes it predictable.
     * The main menu's own items take this path too, so a menu selection and
     * the panel key are one code path. Returns false, changing nothing, for a
     * group with no page registered yet (Track, Mixer).
     */
    bool jumpToRoot(RootGroup group);

    using PageFactory = std::function<std::shared_ptr<UIPage>()>;
    /// Registers what jumpToRoot(group) pushes. Called once at start-up by
    /// initNavigationSystem(); the navigator owns no page constructors itself.
    void setRootGroupFactory(RootGroup group, PageFactory factory);
    bool hasRootGroup(RootGroup group) const;

    /// The group at the bottom of the stack - what the panel's jump LEDs show
    /// (§4.5). RootGroup::None at the main menu.
    RootGroup activeRootGroup() const { return root_group_; }

    std::shared_ptr<UIPage> active() const { return active_; }
    SoftkeyBar* softkeyBar() { return &softkeyBar_; }
    bool canPop() const { return stack_.size() > 1; }
    void refreshSoftkeys();
    size_t depth() const { return stack_.size(); }

    /**
     * @brief Shift modifier: reveals the active page's alternate softkey row.
     *
     * Latched, not held. A touch panel makes hold-and-press awkward with one
     * hand, and holding a physical key while turning the encoder is worse. It
     * is *sticky*: it clears itself after one shifted key is used, so it
     * cannot be left on by accident, which is the usual failure of a plain
     * toggle.
     */
    void toggleShift();
    void setShift(bool on);
    bool isShifted() const { return shifted_; }

    /** True if the active page actually defines an alternate row. */
    bool activePageHasShiftedKeys() const;

    /** Called by the softkey bar after a shifted key fires, to unstick Shift. */
    void notifySoftkeyUsed();

   private:
    UINavigator() = default;
    ~UINavigator() = default;

    // Exits the top page and discards everything above the root without
    // re-entering anything. The caller decides what shows next.
    void unwindToRoot();
    // Enters `page` on top of the stack. `exit_current` is false when the
    // page below has already been exited (after unwindToRoot()).
    void enter(std::shared_ptr<UIPage> page, bool exit_current);
    void layoutContent();

    RootGroup root_group_ = RootGroup::None;
    PageFactory factories_[static_cast<size_t>(RootGroup::Count)]{};

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* shift_chip_ = nullptr;
    lv_obj_t* shift_label_ = nullptr;
    bool shifted_ = false;

    void buildShiftChip();
    void refreshShiftChip();
    static void shiftChipEventCb(lv_event_t* e);
    std::stack<std::shared_ptr<UIPage>> stack_;
    std::shared_ptr<UIPage> active_;
    SoftkeyBar softkeyBar_;
};

}  // namespace wavex_ui
