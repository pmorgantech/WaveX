// WaveX UI Navigation Manager (Stack-Based)
#pragma once

#include "ui_page.h"
#include "ui_softkey_bar.h"

#include <memory>
#include <stack>

namespace wavex_ui {

/**
 * @brief Navigation manager with stack-based page navigation
 *
 * Manages a stack of UI pages with push/pop operations.
 * Automatically handles page lifecycle (onEnter/onExit) and softkey updates.
 */
class UINavigator {
   public:
    /**
     * @brief Get the singleton instance
     */
    static UINavigator& instance() {
        static UINavigator inst;
        return inst;
    }

    /**
     * @brief Push a new page onto the navigation stack
     * @param page Page to push (takes ownership)
     */
    void push(std::shared_ptr<UIPage> page);

    /**
     * @brief Pop the current page from the navigation stack
     * Does nothing if only one page remains (root page)
     */
    void pop();

    /**
     * @brief Get the currently active page
     */
    std::shared_ptr<UIPage> active() const { return active_; }

    /**
     * @brief Get the softkey bar for encoder focus support
     */
    SoftkeyBar* softkeyBar() { return &softkeyBar_; }

    /**
     * @brief Check if navigation stack has more than one page
     */
    bool canPop() const { return stack_.size() > 1; }

    /**
     * @brief Refresh softkeys using the currently active page definition
     */
    void refreshSoftkeys();

    /**
     * @brief Get the current stack depth
     */
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
