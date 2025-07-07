/**
 * @file lv_conf.h
 * Configuration file for LVGL v9.x for ESP32
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/

/* Color depth: 1 (1 byte per pixel), 8 (RGB332), 16 (RGB565), 32 (ARGB8888) */
#define LV_COLOR_DEPTH 16

/*=========================
   MEMORY SETTINGS
 *=========================*/

/* Size of the memory available for `lv_malloc()` in bytes (>= 2kB) */
#define LV_MEM_SIZE (64U * 1024U)  /* 64KB */

/* Set an address for the memory pool instead of allocating it as a normal array. */
#define LV_MEM_ADR 0     /* 0: unused */

/* Instead of an address give a memory allocator that will be called to get a memory pool for LVGL. */
#define LV_MEM_CUSTOM 0

/*===================
   HAL SETTINGS
 *==================*/

/* Default refresh period. LVG will redraw changed areas with this period time */
#define LV_DEF_REFR_PERIOD 5    /* [ms] */

/* Input device read period in milliseconds */
#define LV_INDEV_DEF_READ_PERIOD 5     /* [ms] */

/*========================
 * FEATURE CONFIGURATION
 *========================*/

/* Show some widget. Might be required for the examples. */
#define LV_USE_CALENDAR       1
#define LV_USE_CHART          1
#define LV_USE_KEYBOARD       1
#define LV_USE_LIST           1
#define LV_USE_MENU           1    /* Required for About menu */
#define LV_USE_MSGBOX         1    /* Required for modals */
#define LV_USE_SLIDER         1    /* Required for settings */
#define LV_USE_SWITCH         1    /* Required for toggles */
#define LV_USE_TEXTAREA       1    /* Required for license text */
#define LV_USE_TABLE          1

/*==================
 * FONT USAGE
 *=================*/

/* Montserrat fonts with various sizes in a range 8..48.
 * The font files are generated from: https://fonts.google.com/specimen/Montserrat */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

/* Demonstrate special features */
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0  /*bpp = 3*/
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0  /*Hebrew, Arabic, Persian letters and all their forms*/
#define LV_FONT_SIMSUN_16_CJK            0  /*1000 most common CJK radicals*/

/*Pixel perfect monospace fonts*/
#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

/* Optionally declare custom fonts here.
 * You can use these fonts as default font too and they will be available globally.
 * E.g. #define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(my_font_1) LV_FONT_DECLARE(my_font_2)
 */
#define LV_FONT_CUSTOM_DECLARE

/* Enable the built-in fonts */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*=================
 *  TEXT SETTINGS
 *=================*/

/**
 * Select a character encoding for strings.
 * Your IDE or editor should have the same character encoding
 * - LV_TXT_ENC_UTF8
 * - LV_TXT_ENC_ASCII
 */
#define LV_TXT_ENC LV_TXT_ENC_UTF8

/*===================
 *  LV_OBJ SETTINGS
 *==================*/

/* Enable/disable the animations */
#define LV_USE_ANIMATION 1

/*==================
 *  THEME USAGE
 *================*/

/*Always enable at least on theme*/

/* No theme, you can apply your styles as you need
 * No flags. Set LV_THEME_DEFAULT_FLAG 0 */
#define LV_USE_THEME_BASIC 1

/* Simple to use monochromatic theme */
#define LV_USE_THEME_MONO 1

/* Dark futuristic theme */
#define LV_USE_THEME_DEFAULT 1

/* Set the default theme. Make sure the chosen theme is enabled above.
 * If LV_THEME_DEFAULT_INIT_FUNCTION is NULL then lv_theme_basic_init() will be used. */
#define LV_THEME_DEFAULT_INIT_FUNCTION lv_theme_default_init

/* Enable the themes */
#define LV_THEME_DEFAULT_FLAG LV_THEME_DEFAULT_FLAG_LIGHT

/*===================
 *    EXAMPLES
 *==================*/

/* Enable the examples to be built with the library */
#define LV_BUILD_EXAMPLES 0

/*===================
 *    DEMO USAGE
 *==================*/

/* Show some demos */
#define LV_USE_DEMO_WIDGETS   0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS    0
#define LV_USE_DEMO_MUSIC     0

/*--END OF LV_CONF_H--*/

#endif /*LV_CONF_H*/ 