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

   private:
    UINavigator() = default;
    ~UINavigator() = default;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* content_ = nullptr;
    std::stack<std::shared_ptr<UIPage>> stack_;
    std::shared_ptr<UIPage> active_;
    SoftkeyBar softkeyBar_;
};

}  // namespace wavex_ui
