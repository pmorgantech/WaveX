#include "ui/tca8418_keypad.h"

#include "bsp/esp32_p4_nano.h"
#include "config/hardware_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pin_config.h"
#include "ui/input_dispatcher.h"
#include "ui/input_event.h"
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
#include "esp_tca8418.hxx"
#endif

namespace wavex_ui {

static const char* TAG = "TCA8418";
static TaskHandle_t s_task = nullptr;
// Shutdown handshake: the task talks I2C on a bus shared with the touch
// controller, so killing it mid-transaction would leak the bus mutex and take
// touch down with it permanently.
static volatile bool s_running = false;
static gpio_num_t s_int_gpio = GPIO_NUM_NC;
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
static TCA8418* s_dev = nullptr;
#endif

// Simple example mapping: map TCA keycode -> logical button id
static uint8_t map_keycode_to_button(uint8_t keycode) {
    // Keycode per TCA8418: 1..80 => R/C encoded; adjust as needed
    // Example: return 1 for Select, 2 for Back, 3 for EncoderClick
    switch (keycode) {
        case 1:
            return 1;  // Select
        case 2:
            return 2;  // Back
        case 3:
            return 3;  // EncoderClick
        case 4:
            return 4;  // Shift (BUTTON_SHIFT) - reveals the alternate
                       // softkey row; handled globally in InputDispatcher
        default:
            return 0;  // Unknown
    }
}

static void post_button(bool pressed, uint8_t button_id) {
    if (button_id == 0)
        return;
    InputEvent evt{};
    evt.type = pressed ? InputType::ButtonPress : InputType::ButtonRelease;
    evt.source_id = button_id;
    evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    InputDispatcher::instance().post(evt);
}

// Poll period. The controller debounces in hardware and buffers up to ten
// events, so this only bounds latency, not whether a key is seen at all.
static constexpr uint32_t kPollIntervalMs = 10;

// The FIFO is ten deep; the cap only stops a wedged controller reporting a
// non-zero count forever from spinning this task.
static constexpr int kMaxEventsPerPass = 16;

static void keypad_task(void* arg) {
    ESP_LOGI(TAG, "Keypad task started");

    while (s_running) {
        // The INT line is deliberately not consulted.
        //
        // Nothing configures the controller to drive it: the driver's hw_init()
        // sets the GPIO/keypad/debounce registers but never writes CFG, so the
        // key-event interrupt enable stays at its reset default. Gating reads on
        // INT therefore meant either no key was ever read, or - if INT did
        // assert - a 100% busy-spin, because INT latches until INT_STAT is
        // written back and the asserted branch had no delay. At priority 5
        // pinned to core 1 that starves the UI task on the same core.
        //
        // Polling the event count is the authority instead, which works
        // whatever CFG holds. Clearing INT_STAT is left undone on purpose: the
        // only public way to do it is flush(), which also discards queued
        // events, so calling it would open a window where a key pressed between
        // our last read and the clear is silently dropped. A latched INT line
        // nobody reads is harmless.
        //
        // An interrupt-driven path is still the better design (guide §2/§3) but
        // needs CFG configured and confirmed on the bench; see roadmap
        // § Outstanding hardware verification.
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
        int drained = 0;
        while (s_dev && s_dev->get_event_count() > 0 && drained < kMaxEventsPerPass) {
            const uint8_t event = s_dev->get_key();
            if (event == 0) {
                break;  // count and FIFO disagree; nothing to decode
            }
            drained++;

            // KEY_EVENT_A packs the transition in bit 7 (1 = press) and the
            // key code in bits 0-6. Masking it is not optional: reading the
            // register raw made a press of key 1 arrive as 0x81, which fell
            // through the keycode mapping and was dropped, while its release
            // arrived as 0x01 and was posted as a *press*. Every button
            // therefore fired on release, and chords were unrepresentable.
            const bool pressed = (event & 0x80) != 0;
            const uint8_t keycode = static_cast<uint8_t>(event & 0x7F);
            post_button(pressed, map_keycode_to_button(keycode));
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t tca8418_keypad_start(int int_gpio, uint8_t i2c_addr) {
    if (s_task)
        return ESP_OK;

    // Use BSP I2C bus (shared with touch per pin_config)
    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    if (i2c == nullptr) {
        ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Failed to init BSP I2C");
        i2c = bsp_i2c_get_handle();
    }

    // Create device
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
    s_dev = new TCA8418(i2c, GPIO_NUM_NC, i2c_addr);
    if (!s_dev) {
        ESP_LOGE(TAG, "Failed to create TCA8418 instance");
        return ESP_FAIL;
    }
    if (!s_dev->hw_init(WAVEX_TCA8418_ROWS, WAVEX_TCA8418_COLUMNS)) {
        ESP_LOGE(TAG, "TCA8418 hardware initialization failed");
        delete s_dev;
        s_dev = nullptr;
        return ESP_FAIL;
    }
#else
    (void)i2c_addr;
#endif

    // Configure the INT GPIO if provided (from the pin_config macro in the
    // caller). The task does not read it - see keypad_task() for why - but
    // leaving the pin floating on a controller that may drive it low is worse
    // than parking it as a pulled-up input, and an interrupt-driven path will
    // want it configured exactly like this.
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
    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(keypad_task,
                                            "tca8418_task",
                                            WAVEX_TCA8418_TASK_STACK_SIZE,
                                            nullptr,
                                            WAVEX_TCA8418_TASK_PRIORITY,
                                            &s_task,
                                            1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create keypad task");
        s_running = false;
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
        delete s_dev;
        s_dev = nullptr;
#endif
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tca8418_keypad_stop() {
    s_running = false;
    // Let the task finish any I2C transaction and self-delete before the
    // device object goes away underneath it.
    for (int waited_ms = 0; s_task && waited_ms < 300; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task) {
        ESP_LOGE(TAG, "keypad task did not exit; leaving the device allocated");
        return ESP_ERR_TIMEOUT;
    }
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
    if (s_dev) {
        delete s_dev;
        s_dev = nullptr;
    }
#endif
    s_int_gpio = GPIO_NUM_NC;
    return ESP_OK;
}

}  // namespace wavex_ui
