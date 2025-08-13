#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Initialize display and touch if LovyanGFX is available; otherwise no-op
void lgfx_device_init(void);

// Set backlight brightness 0..255 (no-op if not supported)
void lgfx_device_set_brightness(int brightness);

// Draw a simple splash/boot screen to verify rendering
void lgfx_device_draw_splash(void);

// Non-throwing touch getter, returns true if touched and writes coords
bool lgfx_device_get_touch(unsigned short* x, unsigned short* y);

#ifdef __cplusplus
}
#endif


