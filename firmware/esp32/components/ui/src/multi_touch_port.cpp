#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "ui/multi_touch_input.h"

#include <new>

namespace {
static_assert(CONFIG_ESP_LCD_TOUCH_MAX_POINTS >= wavex_ui::MultiTouchInput::kMaxContacts,
              "WaveX requires all five GT911 contacts, including their tracking IDs");

struct TouchPort {
    wavex_ui::MultiTouchInput input;
    lvgl_port_touch_cfg_t config{};
    lv_timer_t* timer = nullptr;
};

void pollTouch(lv_timer_t* timer) {
    auto* port = static_cast<TouchPort*>(lv_timer_get_user_data(timer));
    esp_lcd_touch_point_data_t points[wavex_ui::MultiTouchInput::kMaxContacts]{};
    uint8_t count = 0;
    // One reader of the BSP's existing controller, never one I2C read per finger.
    const esp_err_t read_result = esp_lcd_touch_read_data(port->config.handle);
    if (read_result != ESP_OK ||
        esp_lcd_touch_get_data(
            port->config.handle, points, &count, wavex_ui::MultiTouchInput::kMaxContacts) !=
            ESP_OK) {
        // Do not leave a pad or drag permanently held after an I2C failure.
        port->input.update(nullptr, 0);
        return;
    }
    wavex_ui::TouchContact contacts[wavex_ui::MultiTouchInput::kMaxContacts]{};
    const float sx = port->config.scale.x ? port->config.scale.x : 1.0f;
    const float sy = port->config.scale.y ? port->config.scale.y : 1.0f;
    for (uint8_t i = 0; i < count; ++i) {
        contacts[i] = {
            points[i].track_id,
            {static_cast<int32_t>(sx * points[i].x), static_cast<int32_t>(sy * points[i].y)}};
    }
    port->input.update(contacts, count);
}
}  // namespace

// Interpose only the public touch registration API used by the BSP. Keeping
// this in first-party sources avoids edits to regenerated managed components
// or dependence on esp_lvgl_port's private touch-context layout.
extern "C" lv_indev_t* __wrap_lvgl_port_add_touch(const lvgl_port_touch_cfg_t* config) {
    if (!config || !config->disp || !config->handle) {
        return nullptr;
    }
    auto* port = new (std::nothrow) TouchPort;
    if (!port) {
        return nullptr;
    }
    port->config = *config;
    lvgl_port_lock(0);
    if (!port->input.init(config->disp)) {
        lvgl_port_unlock();
        delete port;
        return nullptr;
    }
    port->timer = lv_timer_create(pollTouch, LV_DEF_REFR_PERIOD, port);
    if (!port->timer) {
        port->input.deinit();
        lvgl_port_unlock();
        delete port;
        return nullptr;
    }
    lv_indev_t* primary = port->input.pointer(0);
    // Driver data belongs to the per-finger slot; user data owns port teardown.
    lv_indev_set_user_data(primary, port);
    lvgl_port_unlock();
    return primary;
}

extern "C" esp_err_t __wrap_lvgl_port_remove_touch(lv_indev_t* primary) {
    if (!primary) {
        return ESP_ERR_INVALID_ARG;
    }
    lvgl_port_lock(0);
    auto* port = static_cast<TouchPort*>(lv_indev_get_user_data(primary));
    lv_timer_delete(port->timer);
    port->input.update(nullptr, 0);
    port->input.deinit();
    delete port;
    lvgl_port_unlock();
    return ESP_OK;
}
