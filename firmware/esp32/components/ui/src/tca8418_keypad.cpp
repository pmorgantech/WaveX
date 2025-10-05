#include "ui/tca8418_keypad.h"
#include "ui/input_dispatcher.h"
#include "ui/input_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "bsp/esp32_p4_nano.h"
#include "pin_config.h"

#ifdef ESP_PLATFORM
#include "esp_tca8418.h"
#endif

namespace wavex_ui {

static const char* TAG = "TCA8418";
static TaskHandle_t s_task = nullptr;
static gpio_num_t s_int_gpio = GPIO_NUM_NC;
#ifdef ESP_PLATFORM
static tca8418_handle_t s_dev = nullptr;
#endif

// Simple example mapping: map TCA keycode -> logical button id
static uint8_t map_keycode_to_button(uint8_t keycode) {
    // Keycode per TCA8418: 1..80 => R/C encoded; adjust as needed
    // Example: return 1 for Select, 2 for Back, 3 for EncoderClick
    switch (keycode) {
        case 1: return 1; // Select
        case 2: return 2; // Back
        case 3: return 3; // EncoderClick
        default: return 0; // Unknown
    }
}

static void post_button(bool pressed, uint8_t button_id) {
    if (button_id == 0) return;
    InputEvent evt{};
    evt.type = pressed ? InputType::ButtonPress : InputType::ButtonRelease;
    evt.source_id = button_id;
    evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    InputDispatcher::instance().post(evt);
}

static void keypad_task(void* arg) {
    ESP_LOGI(TAG, "Keypad task started");
    while (true) {
        // If INT available, wait for it, otherwise poll periodically
        if (s_int_gpio != GPIO_NUM_NC) {
            // Simple polling of GPIO level; could use ISR+queue for lower latency
            if (gpio_get_level(s_int_gpio) == 0) {
                // INT asserted (active low)
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

    // Drain event FIFO
#ifdef ESP_PLATFORM
    tca8418_event_t ev;
    while (tca8418_get_event(s_dev, &ev) == ESP_OK) {
        bool pressed = (ev.type == TCA8418_EVENT_PRESS);
        uint8_t button = map_keycode_to_button(ev.key);
        post_button(pressed, button);
    }
#endif
    }
}

esp_err_t tca8418_keypad_start(int int_gpio, uint8_t i2c_addr) {
    if (s_task) return ESP_OK;

    // Use BSP I2C bus (shared with touch per pin_config)
    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    if (i2c == nullptr) {
        ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Failed to init BSP I2C");
        i2c = bsp_i2c_get_handle();
    }

    // Create device
#ifdef ESP_PLATFORM
    tca8418_config_t cfg = {
        .i2c_bus = i2c,
        .i2c_addr = i2c_addr,
        .rows = 8,
        .cols = 10,
        .debounce_ms = 5,
        .autorepeat = false,
    };
    ESP_RETURN_ON_ERROR(tca8418_new(&cfg, &s_dev), TAG, "tca8418_new failed");
#else
    (void)i2c_addr;
#endif

    // Configure INT GPIO if provided (from pin_config macro in caller)
    if (int_gpio >= 0) {
        s_int_gpio = (gpio_num_t)int_gpio;
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << s_int_gpio;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config failed");
    }

    // Start task
    BaseType_t ok = xTaskCreatePinnedToCore(keypad_task, "tca8418_task", 4096, nullptr, 5, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create keypad task");
        #ifdef ESP_PLATFORM
        tca8418_del(s_dev);
        s_dev = nullptr;
        #endif
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tca8418_keypad_stop() {
    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    #ifdef ESP_PLATFORM
    if (s_dev) {
        tca8418_del(s_dev);
        s_dev = nullptr;
    }
    #endif
    s_int_gpio = GPIO_NUM_NC;
    return ESP_OK;
}

} // namespace wavex_ui


