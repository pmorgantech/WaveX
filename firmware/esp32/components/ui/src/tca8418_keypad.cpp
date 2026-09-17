#include "ui/tca8418_keypad.h"

#include "bsp/esp32_p4_nano.h"
#include "config/hardware_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui/input_dispatcher.h"
#include "ui/panel/keypad_fifo.h"
#include "ui/panel_key.h"

#include <atomic>

namespace wavex_ui {
namespace {
constexpr const char* TAG = "TCA8418";
std::atomic<TaskHandle_t> s_task{nullptr};
std::atomic<bool> s_running{false}, s_interrupt{false};
i2c_master_dev_handle_t s_device = nullptr;
gpio_num_t s_int_gpio = GPIO_NUM_NC;
bool s_handler = false;
// The endpoint lock serializes ISR notification with endpoint withdrawal.
// The task is never deleted until all possible notifiers have released it.
DRAM_ATTR portMUX_TYPE s_irq_lock = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR TaskHandle_t s_irq_task = nullptr;
std::atomic<uint8_t> s_last_keycode{0};
std::atomic<bool> s_last_pressed{false};
std::atomic<uint32_t> s_events{0}, s_unmapped{0}, s_errors{0}, s_overflows{0};

struct Registers {
    bool read(uint8_t reg, uint8_t& value) {
        return i2c_master_transmit_receive(s_device, &reg, 1, &value, 1, 20) == ESP_OK;
    }
    bool write(uint8_t reg, uint8_t value) {
        const uint8_t bytes[]{reg, value};
        return i2c_master_transmit(s_device, bytes, sizeof(bytes), 20) == ESP_OK;
    }
};
KeypadFifo s_fifo;

bool post_key(bool pressed, uint8_t keycode) {
    const PanelKey key = panelKeyFromKeycode(keycode);
    if (key != PanelKey::None) {
        InputEvent event{};
        event.type = pressed ? InputType::KeyPress : InputType::KeyRelease;
        event.source_id = static_cast<uint8_t>(key);
        event.timestamp_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (!InputDispatcher::instance().post(event))
            return false;
    } else if (pressed) {
        s_unmapped.fetch_add(1, std::memory_order_relaxed);
    }
    if (pressed)
        s_events.fetch_add(1, std::memory_order_relaxed);
    s_last_keycode.store(keycode, std::memory_order_relaxed);
    s_last_pressed.store(pressed, std::memory_order_relaxed);
    return true;
}

void IRAM_ATTR keypad_interrupt(void*) {
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&s_irq_lock);
    if (s_irq_task)
        vTaskNotifyGiveFromISR(s_irq_task, &wake);
    portEXIT_CRITICAL_ISR(&s_irq_lock);
    if (wake)
        portYIELD_FROM_ISR();
}

void remove_interrupt() {
    portENTER_CRITICAL(&s_irq_lock);
    s_irq_task = nullptr;
    portEXIT_CRITICAL(&s_irq_lock);
    if (s_handler) {
        gpio_intr_disable(s_int_gpio);
        gpio_isr_handler_remove(s_int_gpio);
        s_handler = false;
    }
    s_interrupt.store(false);
}

void keypad_task(void*) {
    // Creator publishes the endpoint and installs INT before this wakeup.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Registers io;
    while (s_running.load()) {
        const auto result = s_fifo.service(io, post_key);
        s_errors.store(s_fifo.errors(), std::memory_order_relaxed);
        s_overflows.store(s_fifo.overflows(), std::memory_order_relaxed);
        if (result != KeypadFifo::Result::Idle) {
            // A wedged/held-low device and a full UI queue always yield.
            vTaskDelay(pdMS_TO_TICKS(result == KeypadFifo::Result::IoError ? 100 : 10));
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_interrupt.load() ? 100 : 10));
        }
    }
    remove_interrupt();
    while (!s_fifo.stop(post_key))
        vTaskDelay(pdMS_TO_TICKS(10));
    io.write(0x01, 0);
    i2c_master_bus_rm_device(s_device);
    s_device = nullptr;
    s_task.store(nullptr);  // no device/IRQ access after publishing completion
    vTaskDelete(nullptr);
}
}  // namespace

void tca8418_keypad_last(tca8418_keypad_stats_t* out) {
    if (!out)
        return;
    out->keycode = s_last_keycode.load();
    out->pressed = s_last_pressed.load();
    out->events = s_events.load();
    out->unmapped = s_unmapped.load();
    out->interrupt = s_interrupt.load();
    out->errors = s_errors.load();
    out->overflows = s_overflows.load();
}

esp_err_t tca8418_keypad_start(int int_gpio, uint8_t i2c_addr) {
#if !WAVEX_ESP_BUTTON_MATRIX_ENABLED
    (void)int_gpio;
    (void)i2c_addr;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_task.load())
        return ESP_OK;
    if (int_gpio >= 0 && !GPIO_IS_VALID_GPIO(int_gpio))
        return ESP_ERR_INVALID_ARG;
    auto bus = bsp_i2c_get_handle();
    if (!bus) {
        const auto result = bsp_i2c_init();
        if (result != ESP_OK)
            return result;
        bus = bsp_i2c_get_handle();
    }
    esp_err_t result = i2c_master_probe(bus, i2c_addr, 20);
    if (result != ESP_OK)
        return result;  // missing keypad never takes down touch
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = i2c_addr;
    config.scl_speed_hz = WAVEX_TCA8418_I2C_CLOCK_SPEED;
    result = i2c_master_bus_add_device(bus, &config, &s_device);
    if (result != ESP_OK)
        return result;
    s_fifo = {};
    Registers io;
    if (!s_fifo.configure(io, WAVEX_TCA8418_ROWS, WAVEX_TCA8418_COLUMNS)) {
        i2c_master_bus_rm_device(s_device);
        s_device = nullptr;
        return ESP_FAIL;
    }
    s_int_gpio = static_cast<gpio_num_t>(int_gpio);
    s_running.store(true);
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(keypad_task,
                                "tca8418_task",
                                WAVEX_TCA8418_TASK_STACK_SIZE,
                                nullptr,
                                WAVEX_TCA8418_TASK_PRIORITY,
                                &task,
                                1) != pdPASS) {
        s_running.store(false);
        io.write(0x01, 0);
        i2c_master_bus_rm_device(s_device);
        s_device = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_task.store(task);
    if (int_gpio >= 0 && WAVEX_TCA8418_INTERRUPT_ENABLED) {
        gpio_config_t pins{};
        pins.pin_bit_mask = 1ULL << int_gpio;
        pins.mode = GPIO_MODE_INPUT;
        pins.pull_up_en = GPIO_PULLUP_ENABLE;
        pins.intr_type = GPIO_INTR_DISABLE;
        result = gpio_config(&pins);
        if (result == ESP_OK) {
            result = gpio_install_isr_service(0);
            if (result == ESP_ERR_INVALID_STATE)
                result = ESP_OK;  // BSP owns shared service
        }
        if (result == ESP_OK) {
            portENTER_CRITICAL(&s_irq_lock);
            s_irq_task = task;
            portEXIT_CRITICAL(&s_irq_lock);
            result = gpio_isr_handler_add(s_int_gpio, keypad_interrupt, nullptr);
            s_handler = result == ESP_OK;
            if (s_handler)
                result = gpio_set_intr_type(s_int_gpio, GPIO_INTR_NEGEDGE);
            if (result == ESP_OK)
                result = gpio_intr_enable(s_int_gpio);
        }
        if (result == ESP_OK)
            s_interrupt.store(true);
        else {
            remove_interrupt();
            ESP_LOGW(TAG, "INT unavailable; using polling");
        }
    }
    xTaskNotifyGive(task); // also drains FIFO when INT was already low at startup
    ESP_LOGI(TAG, "Keypad started: %s", s_interrupt.load() ? "INT + fallback poll" : "polling");
    return ESP_OK;
#endif
}

esp_err_t tca8418_keypad_stop() {
    s_running.store(false);
    // Same endpoint lock as ISR: a task completing stop cannot be notified
    // after deletion. Startup/stop API calls are serialized by their UI owner.
    portENTER_CRITICAL(&s_irq_lock);
    if (s_irq_task)
        xTaskNotifyGive(s_irq_task);
    portEXIT_CRITICAL(&s_irq_lock);
    for (unsigned waited = 0; s_task.load() && waited < 500; waited += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    return s_task.load() ? ESP_ERR_TIMEOUT : ESP_OK;
}
}  // namespace wavex_ui
