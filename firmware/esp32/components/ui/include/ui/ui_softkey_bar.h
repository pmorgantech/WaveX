// WaveX UI Softkey Bar Widget
#pragma once

#include <lvgl.h>

#include "ui_softkey.h"

#include <array>

namespace wavex_ui {

/// The 6 buttons fixed at the bottom of the screen, with dynamic labels and
/// touch/encoder support.
class SoftkeyBar {
   public:
    void create(lv_obj_t* parent);
    void setSoftkeys(const std::array<Softkey, NUM_SOFTKEYS>& keys, bool shifted = false);
    lv_obj_t* container() const { return container_; }

    /// delta: +1 for next, -1 for previous.
    void focusNext(int delta);
    void pressFocused();

   private:
    static void event_cb(lv_event_t* e);

    lv_obj_t* container_ = nullptr;
    std::array<lv_obj_t*, NUM_SOFTKEYS> btns_{};
    std::array<lv_obj_t*, NUM_SOFTKEYS> labels_{};
    std::array<Softkey, NUM_SOFTKEYS> keys_{};
    int focused_ = 0;
};

}  // namespace wavex_ui
