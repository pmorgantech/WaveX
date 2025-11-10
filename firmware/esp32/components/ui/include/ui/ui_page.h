// WaveX UI Page Abstraction
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_softkey.h"

#include <memory>
#include <string>

namespace wavex_ui {

/**
 * @brief Base class for all UI pages/screens
 *
 * Each page manages its own LVGL objects, state, and input handling.
 * Pages can be pushed/popped from a navigation stack.
 */
class UIPage {
   public:
    virtual ~UIPage() = default;

    /**
     * @brief Get the page name for debugging/logging
     */
    virtual const char* name() const = 0;

    /**
     * @brief Called when page becomes active
     * @param parent LVGL parent object (usually screen)
     */
    virtual void onEnter(lv_obj_t* parent) = 0;

    /**
     * @brief Called when page becomes inactive
     * Default implementation does nothing
     */
    virtual void onExit() {}

    /**
     * @brief Handle input events for this page
     * @param evt Input event
     */
    virtual void onInput(const InputEvent& evt) {}

    /**
     * @brief Get softkey configuration for this page
     * @return Array of 6 softkeys (empty by default)
     */
    virtual std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() {
        return {};  // Default: no softkeys
    }

    /**
     * @brief Get the root LVGL object for this page
     */
    lv_obj_t* root() const { return root_; }

   protected:
    lv_obj_t* root_ = nullptr;
};

}  // namespace wavex_ui
