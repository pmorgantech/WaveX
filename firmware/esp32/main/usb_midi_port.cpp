#include "config/hardware_config.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "usb_midi_port_internal.h"
#include "usb_midi_task.h"

namespace wavex_midi {
namespace {
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
UsbPortStatus status;
UsbMode requested = UsbMode::Device;
#if WAVEX_ESP_USB_MIDI_ENABLED && (WAVEX_USB_MIDI_INPUT_ENABLED || WAVEX_USB_MIDI_OUTPUT_ENABLED)
constexpr bool enabled = true;
#else
constexpr bool enabled = false;
#endif
}  // namespace
UsbPortStatus ReadUsbPort() {
    portENTER_CRITICAL(&mutex);
    const auto result = status;
    portEXIT_CRITICAL(&mutex);
    return result;
}
void SetUsbConnection(UsbConnection value) {
    portENTER_CRITICAL(&mutex);
    status.connection = value;
    portEXIT_CRITICAL(&mutex);
}
bool SaveUsbMode(UsbMode mode) {
    if (!enabled || (mode != UsbMode::Device && mode != UsbMode::Host))
        return false;
    portENTER_CRITICAL(&mutex);
    const bool accepted = !status.saving;
    if (accepted) {
        requested = mode;
        status.saving = true;
    }
    portEXIT_CRITICAL(&mutex);
    return accepted;
}
void ServiceUsbSettings() {
    portENTER_CRITICAL(&mutex);
    const bool pending = status.saving;
    const auto mode = requested;
    portEXIT_CRITICAL(&mutex);
    if (!pending)
        return;
    // Never erase NVS on failure: calibration shares this partition.
    auto result = nvs_flash_init();
    nvs_handle_t handle;
    if (result == ESP_OK)
        result = nvs_open("wavex_midi", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, "usb_mode", static_cast<uint8_t>(mode));
        if (result == ESP_OK)
            result = nvs_commit(handle);
        nvs_close(handle);
    }
    portENTER_CRITICAL(&mutex);
    if (result == ESP_OK)
        status.saved = mode;
    status.save_result = result;
    status.saving = false;
    portEXIT_CRITICAL(&mutex);
}
esp_err_t StartUsbPort() {
    uint8_t mode = 0;
    auto result = nvs_flash_init();
    nvs_handle_t handle;
    if (result == ESP_OK) {
        result = nvs_open("wavex_midi", NVS_READONLY, &handle);
        if (result == ESP_OK) {
            result = nvs_get_u8(handle, "usb_mode", &mode);
            nvs_close(handle);
        }
    }
    if (result == ESP_ERR_NVS_NOT_FOUND)
        result = ESP_OK;
    if (result == ESP_OK && mode > 1)
        result = ESP_ERR_INVALID_RESPONSE;
    if (result != ESP_OK)
        mode = 0;
    portENTER_CRITICAL(&mutex);
    status.active = status.saved = static_cast<UsbMode>(mode);
    status.save_result = result;
    portEXIT_CRITICAL(&mutex);
    if (!enabled)
        return ESP_OK;
    SetUsbConnection(UsbConnection::Starting);
    result = mode == 1 ? StartUsbHost() : usb_midi_task_start();
    if (result != ESP_OK)
        SetUsbConnection(UsbConnection::Error);
    return result;
}
}  // namespace wavex_midi
