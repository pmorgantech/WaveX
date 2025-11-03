#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
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
    esp_err_t initTouchController();
    esp_err_t startLvglTick();
    void stopLvglTick();

    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    lv_display_t* display_ = nullptr;
    esp_timer_handle_t lvgl_tick_timer_handle_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_handle_ = nullptr;
};

} // namespace wavex_ui

