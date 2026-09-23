#pragma once
#include "esp_err.h"

#include <cstdint>

namespace wavex_midi {
enum class UsbMode : uint8_t { Device, Host };
enum class UsbConnection : uint8_t { Disabled, Starting, Waiting, Connected, Unsupported, Error };
struct UsbPortStatus {
    UsbMode active = UsbMode::Device;
    UsbMode saved = UsbMode::Device;
    UsbConnection connection = UsbConnection::Disabled;
    bool saving = false;
    esp_err_t save_result = ESP_OK;
};
UsbPortStatus ReadUsbPort();
// Queues an NVS write; the application main loop owns persistence, never LVGL.
bool SaveUsbMode(UsbMode mode);
}  // namespace wavex_midi
