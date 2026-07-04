/**
 * @file usb_midi_task.h
 * @brief USB MIDI device input task (roadmap Phase 1 item 8)
 *
 * Presents the ESP32-P4's native USB-OTG port as a class-compliant USB
 * MIDI device (via the esp_tinyusb device stack), so a host DAW/computer
 * can send notes into WaveX. Received events go through the same shared
 * parser and inter-MCU forwarding as DIN MIDI (midi_task.cpp).
 *
 * Compiled out when WAVEX_ESP_USB_MIDI_ENABLED or
 * WAVEX_USB_MIDI_INPUT_ENABLED is 0 - the init function then exists as a
 * no-op returning ESP_OK. Also requires CONFIG_TINYUSB_MIDI_COUNT=1
 * (sdkconfig) to compile the TinyUSB MIDI class in.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the TinyUSB driver (MIDI descriptor) and start the
 *        reader task.
 *
 * Requires inter_mcu_init()/inter_mcu_start() to have run - forwarded
 * notes are dropped (with a log) until the link is up.
 *
 * @return ESP_OK on success (also when the feature is compiled out)
 */
esp_err_t usb_midi_task_start(void);

/**
 * @brief Stop the reader task and uninstall the TinyUSB driver.
 *
 * @return ESP_OK on success
 */
esp_err_t usb_midi_task_stop(void);

#ifdef __cplusplus
}
#endif
