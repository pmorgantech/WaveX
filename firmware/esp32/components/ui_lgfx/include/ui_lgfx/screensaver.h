#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Set screensaver timeout in minutes (0 = disabled)
void screensaver_set_timeout(int minutes);

// Notify screensaver of user activity (call this on any input event)
void screensaver_reset_timer(void);

// Initialize screensaver system
void screensaver_init(void);

#ifdef __cplusplus
}
#endif
