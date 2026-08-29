/**
 * Host-preview LVGL configuration.
 *
 * Mirrors the DEVICE configuration where it affects appearance, so a render
 * from this harness is a truthful preview of the hardware: RGB565, the same
 * Montserrat sizes the firmware compiles in (sdkconfig
 * CONFIG_LV_FONT_MONTSERRAT_*), default body font 18 (ui_theme.h
 * UI_FONT_NORMAL). Divergences are host conveniences only: a big memory pool
 * (no 128 KiB budget to respect off-device) and no logging.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* Plenty of pool on the host; the device uses 128 KiB. */
#define LV_MEM_SIZE (8 * 1024 * 1024)

#define LV_USE_LOG 0

/* Font set exactly as compiled into the firmware. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_DEFAULT (&lv_font_montserrat_18)

#endif /* LV_CONF_H */
