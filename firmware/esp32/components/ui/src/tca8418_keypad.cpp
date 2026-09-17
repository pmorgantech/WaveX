#include "ui/tca8418_keypad.h"

#include "bsp/esp32_p4_nano.h"
#include "config/hardware_config.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/input_dispatcher.h"
#include "ui/input_event.h"
#include "ui/panel_key.h"
#include "ui/tca8418_controller.h"

#include <atomic>

namespace wavex_ui {

static const char* TAG = "TCA8418";
static std::atomic<TaskHandle_t> s_task{nullptr};
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_irq_active{false};
static std::atomic<uint32_t> s_io_errors{0}, s_overflows{0};

// What the Diagnostics ▸ Panel tab shows: the last raw keycode the matrix
// reported, before the WAVEX_KEYCODE_* map, so an unmapped key still says
// which row and column it is on - which is how the map gets verified.
static std::atomic<uint8_t> s_last_event{0};
static std::atomic<uint32_t> s_events{0};
static std::atomic<uint32_t> s_unmapped{0};

static void post_key(bool pressed, uint8_t keycode) {
    s_last_event.store(static_cast<uint8_t>(keycode | (pressed ? 0x80 : 0)),
                       std::memory_order_relaxed);
    if (pressed) {
        s_events.fetch_add(1, std::memory_order_relaxed);
    }
    const PanelKey key = panelKeyFromKeycode(keycode);
    if (key == PanelKey::None) {
        // Not dropped silently: the Panel tab counts these, and the keycode
        // above says where the key is.
        if (pressed) {
            s_unmapped.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(TAG,
                     "Unmapped keycode %u (row %u col %u)",
                     keycode,
                     (keycode - 1) / 10,
                     (keycode - 1) % 10);
        }
        return;
    }
    InputEvent evt{};
    evt.type = pressed ? InputType::KeyPress : InputType::KeyRelease;
    evt.source_id = static_cast<uint8_t>(key);
    evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    InputDispatcher::instance().post(evt);
}

#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
namespace {
// start/stop are serialized by the UI lifecycle. After start, only the keypad
// task accesses this device. stop waits for its I2C/ISR cleanup before removal.
struct Registers {
    i2c_master_dev_handle_t device = nullptr;
    bool read(uint8_t reg, uint8_t& value) {
        return i2c_master_transmit_receive(
                   device, &reg, 1, &value, 1, WAVEX_TCA8418_TRANSFER_TIMEOUT_MS) == ESP_OK;
    }
    bool write(uint8_t reg, uint8_t value) {
        const uint8_t bytes[] = {reg, value};
        return i2c_master_transmit(
                   device, bytes, sizeof(bytes), WAVEX_TCA8418_TRANSFER_TIMEOUT_MS) == ESP_OK;
    }
} s_bus;
Tca8418Controller s_controller;
gpio_num_t s_int_gpio = GPIO_NUM_NC;
// The service is installed by the keypad task on core 1, with flags 0. Never
// uninstall a process-wide service; another driver may subsequently share it.
bool s_owns_gpio_service = false;

void keypad_isr(void* task) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(task), &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

bool enable_irq() {
    if (!WAVEX_TCA8418_INTERRUPT_ENABLED || s_int_gpio == GPIO_NUM_NC)
        return false;
    if (!s_owns_gpio_service) {
        // An existing service may be IRAM-only. Our callback is not: keep
        // polling rather than attach a flash callback with unknown flags.
        if (gpio_install_isr_service(0) != ESP_OK)
            return false;
        s_owns_gpio_service = true;
    }
    if (gpio_isr_handler_add(s_int_gpio, keypad_isr, xTaskGetCurrentTaskHandle()) != ESP_OK)
        return false;
    if (gpio_set_intr_type(s_int_gpio, GPIO_INTR_NEGEDGE) != ESP_OK ||
        gpio_intr_enable(s_int_gpio) != ESP_OK || !s_controller.interrupts(s_bus, true)) {
        gpio_intr_disable(s_int_gpio);
        gpio_isr_handler_remove(s_int_gpio);
        s_controller.interrupts(s_bus, false);
        return false;
    }
    return true;
}

void keypad_task(void*) {
    const bool irq = enable_irq();
    s_irq_active.store(irq);
    ESP_LOGI(TAG, "Keypad: %s", irq ? "interrupt + fallback poll" : "10 ms polling fallback");
    while (s_running.load()) {
        // Always service once at startup, including an already-low INT line.
        const auto result = s_controller.service(s_bus, post_key);
        if (result.overflow)
            s_overflows.fetch_add(1, std::memory_order_relaxed);
        const bool failed = result.state == Tca8418Controller::State::IoError ||
                            result.state == Tca8418Controller::State::InvalidEvent;
        if (failed)
            s_io_errors.fetch_add(1, std::memory_order_relaxed);
        if (failed || result.state == Tca8418Controller::State::Pending) {
            // Always yield after a capped/error pass, even under an IRQ storm.
            vTaskDelay(failed ? pdMS_TO_TICKS(10) : 1);
        } else {
            vTaskDelay(1);  // bound CPU use even if a noisy IRQ keeps notifying
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(irq ? 100 : 10));
        }
    }
    if (irq) {
        // Service and task are both on core 1: removal completes before the
        // task handle can die, so no ISR can notify a freed task.
        gpio_intr_disable(s_int_gpio);
        gpio_isr_handler_remove(s_int_gpio);
    }
    s_irq_active.store(false);
    s_controller.interrupts(s_bus, false);
    s_controller.releaseHeld(post_key);
    s_task.store(nullptr);  // no device/controller access after this handoff
    vTaskDelete(nullptr);
}
}  // namespace
#endif

void tca8418_keypad_last(tca8418_keypad_stats_t* out) {
    if (!out)
        return;
    const auto last = s_last_event.load(std::memory_order_relaxed);
    out->keycode = last & 0x7F;
    out->pressed = (last & 0x80) != 0;
    out->events = s_events.load(std::memory_order_relaxed);
    out->unmapped = s_unmapped.load(std::memory_order_relaxed);
    out->running = s_running.load() && s_task.load() != nullptr;
    out->irq_active = s_irq_active.load();
    out->io_errors = s_io_errors.load(std::memory_order_relaxed);
    out->overflows = s_overflows.load(std::memory_order_relaxed);
}

esp_err_t tca8418_keypad_start(int int_gpio, uint8_t i2c_addr) {
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
    if (s_task.load())
        return s_running.load() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (s_bus.device)  // Previous stop timed out; reclaim only through stop().
        return ESP_ERR_INVALID_STATE;
    if (int_gpio < -1 || (int_gpio >= 0 && !GPIO_IS_VALID_GPIO(int_gpio)) || i2c_addr < 0x08 ||
        i2c_addr > 0x77)
        return ESP_ERR_INVALID_ARG;
    auto bus = bsp_i2c_get_handle();
    if (!bus) {
        const esp_err_t err = bsp_i2c_init();
        if (err != ESP_OK)
            return err;
        bus = bsp_i2c_get_handle();
    }
    if (!bus)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_master_probe(bus, i2c_addr, WAVEX_TCA8418_TRANSFER_TIMEOUT_MS);
    if (err != ESP_OK)
        return err;  // An absent panel must not abort display/touch startup.
    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = i2c_addr;
    cfg.scl_speed_hz = WAVEX_TCA8418_I2C_CLOCK_SPEED;
    err = i2c_master_bus_add_device(bus, &cfg, &s_bus.device);
    if (err != ESP_OK)
        return err;
    auto cleanup = [&] {
        if (i2c_master_bus_rm_device(s_bus.device) == ESP_OK)
            s_bus.device = nullptr;
        s_int_gpio = GPIO_NUM_NC;
    };
    if (!s_controller.init(s_bus, WAVEX_TCA8418_ROWS, WAVEX_TCA8418_COLUMNS)) {
        cleanup();
        return ESP_FAIL;
    }
    s_int_gpio = static_cast<gpio_num_t>(int_gpio);
    if (s_int_gpio != GPIO_NUM_NC) {
        gpio_config_t io{};
        io.pin_bit_mask = 1ULL << int_gpio;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        err = gpio_config(&io);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }
    s_running.store(true);
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(keypad_task,
                                "tca8418_task",
                                WAVEX_TCA8418_TASK_STACK_SIZE,
                                nullptr,
                                WAVEX_TCA8418_TASK_PRIORITY,
                                &handle,
                                1) != pdPASS) {
        s_running.store(false);
        cleanup();
        return ESP_ERR_NO_MEM;
    }
    s_task.store(handle);
    return ESP_OK;
#else
    (void)int_gpio;
    (void)i2c_addr;
    return ESP_OK;
#endif
}

esp_err_t tca8418_keypad_stop() {
    s_running.store(false);
    // Never delete another task during an I2C operation on the touch bus.
    // The notification wait is bounded, so no potentially stale task handle
    // needs to be dereferenced here merely to wake shutdown.
    for (int waited_ms = 0; s_task.load() && waited_ms < 300; waited_ms += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (s_task.load())
        return ESP_ERR_TIMEOUT;
#if defined(ESP_PLATFORM) && WAVEX_ESP_BUTTON_MATRIX_ENABLED
    if (s_bus.device) {
        const esp_err_t err = i2c_master_bus_rm_device(s_bus.device);
        if (err != ESP_OK)
            return err;
        s_bus.device = nullptr;
    }
    s_int_gpio = GPIO_NUM_NC;
#endif
    return ESP_OK;
}

}  // namespace wavex_ui
