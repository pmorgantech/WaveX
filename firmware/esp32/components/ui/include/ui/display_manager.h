#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace wavex_ui {

class DisplayManager {
   public:
    static DisplayManager& instance();

    esp_err_t init();
    void deinit();

    lv_display_t* display() const { return display_; }

    esp_err_t panelHandle(esp_lcd_panel_handle_t* out);

   private:
    DisplayManager() = default;

    esp_err_t initLvglDisplay();

    // No touch handle or tick timer here on purpose: the BSP creates and
    // registers the GT911 indev, and esp_lvgl_port runs the LVGL tick. This
    // class owning either meant a second driver on the same controller and a
    // doubled tick.
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    lv_display_t* display_ = nullptr;
};

}  // namespace wavex_ui
