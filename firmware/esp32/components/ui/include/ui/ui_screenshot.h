#pragma once

// Serial screenshot capture (debug builds only - WAVEX_ESP_SCREENSHOT_DEBUG).
//
// The debug console (ui_console.cpp) receives "WAVEX-SCREENSHOT" or
// "WAVEX-DBG <seq> SCREENSHOT" and calls wavex_screenshot_request(). The next
// wavex_screenshot_poll() call - made once per UI-task loop pass - captures
// the active LVGL screen under the LVGL lock into PSRAM via lv_snapshot, and
// the console task's wavex_screenshot_service() then prints it as
// RLE-compressed, base64-encoded RGB565 between BEGIN/END markers, so the
// UI task is never blocked by the (seconds-long at 115200 baud) dump.
// scripts/esp32_screenshot.py triggers a capture and decodes it to PNG.

#include "config/logging_config.h"

#if WAVEX_ESP_SCREENSHOT_DEBUG

// Console task: ask for a capture. False if one is already in flight.
bool wavex_screenshot_request();

// Call once per UI-task loop pass. Cheap when idle (one flag check).
// Manages its own LVGL locking; call WITHOUT the LVGL lock held.
void wavex_screenshot_poll();

// Console task: print a captured screen, if one is waiting. Returns true if
// it printed (or reported a failure) - i.e. the request has concluded.
bool wavex_screenshot_service();

#else

static inline bool wavex_screenshot_request() {
    return false;
}
static inline void wavex_screenshot_poll() {}
static inline bool wavex_screenshot_service() {
    return false;
}

#endif
