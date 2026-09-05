// WaveX UI Softkey Bar Widget
#pragma once

#include <lvgl.h>

#include "ui_softkey.h"

#include <array>

namespace wavex_ui {

/// The 6 buttons fixed at the bottom of the screen, with dynamic labels,
/// pressed by touch or by the panel's SOFT1..6 keys.
class SoftkeyBar {
   public:
    void create(lv_obj_t* parent);
    void setSoftkeys(const std::array<Softkey, NUM_SOFTKEYS>& keys, bool shifted = false);
    lv_obj_t* container() const { return container_; }

    /**
     * @brief Fire softkey `index` (0..5) exactly as a touch on its button would.
     *
     * Same path as the touch event: the row currently shown (shifted or not),
     * notifySoftkeyUsed() before the callback, the callback deferred through
     * lv_async_call. A disabled or empty key does nothing and returns false.
     * The panel's SOFT keys land here from InputDispatcher.
     */
    bool press(int index);

    /// Read-only view for the debug console's STATE reply: the key as last
    /// set, and its button's centre in screen coordinates (false when the
    /// bar is not built), so a host can TAP it through the real touch path.
    const Softkey& key(int index) const { return keys_[index]; }
    bool buttonCenter(int index, int32_t* x, int32_t* y) const;

   private:
    static void event_cb(lv_event_t* e);

    lv_obj_t* container_ = nullptr;
    std::array<lv_obj_t*, NUM_SOFTKEYS> btns_{};
    std::array<lv_obj_t*, NUM_SOFTKEYS> labels_{};
    std::array<Softkey, NUM_SOFTKEYS> keys_{};
};

}  // namespace wavex_ui
