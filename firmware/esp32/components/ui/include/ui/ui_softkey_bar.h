// WaveX UI Softkey Bar Widget
#pragma once

#include <lvgl.h>

#include "ui_softkey.h"

#include <array>

namespace wavex_ui {

/**
 * @brief Softkey bar widget for bottom navigation
 *
 * Creates 6 buttons at the bottom of the screen with dynamic labels
 * and touch/encoder support.
 */
class SoftkeyBar {
   public:
    /**
     * @brief Create the softkey bar
     * @param parent LVGL parent object
     */
    void create(lv_obj_t* parent);

    /**
     * @brief Update softkey labels and callbacks
     * @param keys Array of 6 softkey definitions
     */
    void setSoftkeys(const std::array<Softkey, NUM_SOFTKEYS>& keys);

    /**
     * @brief Get the container object
     */
    lv_obj_t* container() const { return container_; }

    /**
     * @brief Move focus to next/previous softkey
     * @param delta Direction (+1 for next, -1 for previous)
     */
    void focusNext(int delta);

    /**
     * @brief Press the currently focused softkey
     */
    void pressFocused();

   private:
    static void event_cb(lv_event_t* e);

    lv_obj_t* container_ = nullptr;
    std::array<lv_obj_t*, NUM_SOFTKEYS> btns_{};
    std::array<lv_obj_t*, NUM_SOFTKEYS> labels_{};
    std::array<Softkey, NUM_SOFTKEYS> keys_{};
    int focused_ = 0;  ///< Currently focused softkey index
};

}  // namespace wavex_ui
