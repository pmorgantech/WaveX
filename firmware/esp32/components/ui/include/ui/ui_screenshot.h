#pragma once

// Serial screenshot capture (debug builds only - WAVEX_ESP_SCREENSHOT_DEBUG).
//
// A small listener task watches the console UART for the token
// "WAVEX-SCREENSHOT". The next wavex_screenshot_poll() call - made once per
// UI-task loop pass - captures the active LVGL screen under the LVGL lock
// into PSRAM via lv_snapshot, and the listener task then prints it as
// RLE-compressed, base64-encoded RGB565 between BEGIN/END markers, so the
// UI task is never blocked by the (seconds-long at 115200 baud) dump.
// scripts/esp32_screenshot.py triggers a capture and decodes it to PNG,
// reading through logs/esp32.log so it coexists with the serial logger.

#include "config/logging_config.h"

#if WAVEX_ESP_SCREENSHOT_DEBUG

// Call once per UI-task loop pass. Cheap when idle (one flag check).
// Manages its own LVGL locking; call WITHOUT the LVGL lock held.
void wavex_screenshot_poll();

#else

static inline void wavex_screenshot_poll() {}

#endif
