#pragma once
#include "usb_midi_port.h"
namespace wavex_midi {
esp_err_t StartUsbPort();
void ServiceUsbSettings();
void SetUsbConnection(UsbConnection state);
esp_err_t StartUsbHost();
}  // namespace wavex_midi
