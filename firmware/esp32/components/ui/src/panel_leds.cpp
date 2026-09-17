#include "ui/panel_leds.h"

#include "bsp/esp32_p4_nano.h"
#include "config/pin_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/display_manager.h"
#include "ui/input_dispatcher.h"
#include "ui/ui_navigator.h"

#include <cstring>

namespace wavex_ui {
namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
PanelLedFrame s_desired;
PanelLedStats s_stats;
bool s_enabled = false;
uint32_t s_request = 0, s_ack = 0;
i2c_master_bus_handle_t s_requested_bus = nullptr;
#if WAVEX_ESP_PANEL_LEDS_ENABLED
Pca9956bController s_controller;
struct Backend {
    i2c_master_dev_handle_t devices[WAVEX_PCA9956B_POPULATED_DEVICES]{};
    bool pins = false;
    bool blank(bool value) {
        return pins && gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_PCA9956B_OE),
                                      value ? 1 : 0) == ESP_OK;
    }
    bool reset(bool asserted) {
        return pins && gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_PCA9956B_RESET),
                                      asserted ? 0 : 1) == ESP_OK;
    }
    bool write(unsigned device, uint8_t reg, const uint8_t* data, size_t count) {
        uint8_t bytes[25];
        if (device >= WAVEX_PCA9956B_POPULATED_DEVICES || count > 24)
            return false;
        bytes[0] = static_cast<uint8_t>(reg | (count > 1 ? 0x80 : 0));
        std::memcpy(bytes + 1, data, count);
        return i2c_master_transmit(
                   devices[device], bytes, count + 1, WAVEX_PANEL_LED_TRANSFER_TIMEOUT_MS) ==
               ESP_OK;
    }
    bool read(unsigned device, uint8_t reg, uint8_t* data, size_t count) {
        reg = static_cast<uint8_t>(reg | (count > 1 ? 0x80 : 0));
        return device < WAVEX_PCA9956B_POPULATED_DEVICES &&
               i2c_master_transmit_receive(
                   devices[device], &reg, 1, data, count, WAVEX_PANEL_LED_TRANSFER_TIMEOUT_MS) ==
                   ESP_OK;
    }
    bool open(i2c_master_bus_handle_t bus) {
        if (!pins) {
            if (gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_PCA9956B_OE), 1) != ESP_OK ||
                gpio_set_level(static_cast<gpio_num_t>(WAVEX_ESP_PCA9956B_RESET), 1) != ESP_OK)
                return false;
            gpio_config_t cfg{};
            cfg.pin_bit_mask = (1ULL << WAVEX_ESP_PCA9956B_OE) | (1ULL << WAVEX_ESP_PCA9956B_RESET);
            cfg.mode = GPIO_MODE_OUTPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            if (gpio_config(&cfg) != ESP_OK)
                return false;
            pins = true;
        }
        if (!blank(true) || !reset(false) || !bus)
            return false;
        for (unsigned i = 0; i < WAVEX_PCA9956B_POPULATED_DEVICES; ++i) {
            if (devices[i])
                continue;
            i2c_device_config_t cfg{};
            cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            cfg.device_address = i == 0 ? WAVEX_PCA9956B_I2C_ADDR_0 : WAVEX_PCA9956B_I2C_ADDR_1;
            cfg.scl_speed_hz = WAVEX_PCA9956B_I2C_CLOCK_HZ;
            if (i2c_master_bus_add_device(bus, &cfg, &devices[i]) != ESP_OK)
                return false;
        }
        return true;
    }
    bool close() {
        if (pins) {
            blank(true);
            reset(false);
        }
        bool ok = true;
        for (auto& device: devices) {
            if (!device)
                continue;
            if (i2c_master_bus_rm_device(device) == ESP_OK)
                device = nullptr;
            else
                ok = false;
        }
        return ok;
    }
} s_backend;
bool s_open = false;
uint32_t s_open_retry = 0, s_open_errors = 0;
#endif
}  // namespace

void panel_leds_start() {
    const auto bus = bsp_i2c_get_handle();
    portENTER_CRITICAL(&s_mux);
    s_requested_bus = bus;
    s_desired = {};  // A restart must not expose a previous navigation frame.
    s_enabled = WAVEX_ESP_PANEL_LEDS_ENABLED;
    ++s_request;
    portEXIT_CRITICAL(&s_mux);
}
esp_err_t panel_leds_stop() {
    portENTER_CRITICAL(&s_mux);
    s_enabled = false;
    const uint32_t request = ++s_request;
    portEXIT_CRITICAL(&s_mux);
    for (unsigned i = 0; i < 20; ++i) {
        portENTER_CRITICAL(&s_mux);
        const bool done = s_ack == request;
        portEXIT_CRITICAL(&s_mux);
        if (done)
            return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}
void panel_leds_publish_ui() {
    PanelLedInputs input;
    input.held_buttons = InputDispatcher::instance().heldPanelButtons();
    lvgl_port_lock(portMAX_DELAY);
    auto& nav = UINavigator::instance();
    input.root = nav.activeRootGroup();
    input.blank = DisplayManager::instance().screenBlanked();
    for (const auto& item: kPanelMenuButtons)
        if (nav.hasRootGroup(item.group))
            input.available_roots |= 1u << static_cast<unsigned>(item.group);
    if (auto* bar = nav.softkeyBar()) {
        for (int i = 0; i < NUM_SOFTKEYS; ++i) {
            const auto& key = bar->key(i);
            if (!key.label.empty() && key.enabled && key.onPress)
                input.enabled_softkeys |= static_cast<uint8_t>(1u << i);
            if (key.active)
                input.active_softkeys |= static_cast<uint8_t>(1u << i);
        }
    }
    lvgl_port_unlock();
    const auto frame = makePanelLedFrame(input);
    portENTER_CRITICAL(&s_mux);
    s_desired = frame;
    portEXIT_CRITICAL(&s_mux);
}
PanelLedStats panel_leds_stats() {
    portENTER_CRITICAL(&s_mux);
    const auto result = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return result;
}
void panel_leds_service() {
    portENTER_CRITICAL(&s_mux);
    const bool enabled = s_enabled;
    const auto request = s_request;
    const auto bus = s_requested_bus;
    const auto frame = s_desired;
    portEXIT_CRITICAL(&s_mux);
    PanelLedStats stats;
    bool stopped = !enabled;
#if WAVEX_ESP_PANEL_LEDS_ENABLED
    const auto now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!enabled) {
        if (s_backend.pins)
            s_controller.stop(s_backend);
        stopped = s_backend.close();
        if (stopped)
            s_open = false;
    } else {
        if (!s_open && static_cast<int32_t>(now - s_open_retry) >= 0) {
            s_open = s_backend.open(bus);
            if (!s_open) {
                ++s_open_errors;
                s_open_retry = now + WAVEX_PANEL_LED_RETRY_MS;
            }
        }
        if (s_open)
            s_controller.service(s_backend, frame, now);
    }
    stats = {s_controller.state(),
             s_controller.frames(),
             s_controller.errors() + s_open_errors,
             s_controller.faultStatus()};
    if (enabled && !s_open)
        stats.state = Pca9956bController::State::Retry;
#else
    (void)bus;
    (void)frame;
#endif
    portENTER_CRITICAL(&s_mux);
    s_stats = stats;
    if (stopped && request == s_request)
        s_ack = request;
    portEXIT_CRITICAL(&s_mux);
}
void panel_leds_shutdown() {
    portENTER_CRITICAL(&s_mux);
    s_enabled = false;
    portEXIT_CRITICAL(&s_mux);
    panel_leds_service();
}
}  // namespace wavex_ui
