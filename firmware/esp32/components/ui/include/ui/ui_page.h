// WaveX UI Page Abstraction
#pragma once

#include <lvgl.h>

#include "input_event.h"
#include "ui_softkey.h"

#include <memory>
#include <string>

namespace wavex_ui {

/// Base class for all UI pages/screens. Each page manages its own LVGL
/// objects, state, and input handling, and can be pushed/popped from a
/// navigation stack (see UINavigator).
class UIPage {
   public:
    virtual ~UIPage() = default;

    virtual const char* name() const = 0;
    virtual void onEnter(lv_obj_t* parent) = 0;
    virtual void onExit() {}
    virtual void onInput(const InputEvent& /*evt*/) {}

    virtual std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() {
        return {};  // Default: no softkeys
    }

    /**
     * @brief Alternate softkey row, revealed while Shift is latched.
     *
     * Default is empty, which the navigator reads as "this page has no
     * alternates" - Shift then does nothing visible rather than blanking the
     * row, so pressing it on a page that does not use it is harmless.
     */
    virtual std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() { return {}; }

    lv_obj_t* root() const { return root_; }

   protected:
    lv_obj_t* root_ = nullptr;
};

}  // namespace wavex_ui
