#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui/screen_blanker.h"

#include <atomic>
#include <cstdint>

namespace wavex_ui {

/// Owns the LCD panel handle and the LVGL display it drives. Does not own
/// touch input or the LVGL tick - see the note on panel_handle_/display_
/// below for why those live elsewhere.
class DisplayManager {
   public:
    static DisplayManager& instance();

    esp_err_t init();
    void deinit();

    lv_display_t* display() const { return display_; }

    esp_err_t panelHandle(esp_lcd_panel_handle_t* out);

    /// Sets the normal (awake) backlight level. A blanked panel keeps this
    /// value and restores it on the next user interaction.
    esp_err_t setBrightness(uint8_t percent);

    /// Safe from the LVGL input task and the UI task: records activity only.
    /// The UI task performs the I2C backlight write in serviceScreenBlanker().
    void noteUserActivity();

    /// UI-task service point for the five-minute blank/wake policy.
    void serviceScreenBlanker();

   private:
    DisplayManager() = default;

    esp_err_t initLvglDisplay();
    static void touchActivityEventCb(lv_event_t* event);
    static uint32_t nowMs();

    // No touch handle or tick timer here on purpose: the BSP creates and
    // registers the GT911 indev, and esp_lvgl_port runs the LVGL tick. This
    // class owning either meant a second driver on the same controller and a
    // doubled tick.
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    lv_display_t* display_ = nullptr;
    ScreenBlanker screen_blanker_;
    std::atomic<uint32_t> activity_ms_{0};
    uint32_t last_observed_activity_ms_ = 0;
    uint8_t brightness_percent_ = 100;
};

}  // namespace wavex_ui
