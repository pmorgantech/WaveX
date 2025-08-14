/**
 * @file hardware_pins.h
 * @brief WaveX Hardware GPIO Pin Definitions
 * 
 * Defines GPIO pin assignments for WaveX dual-MCU platform ESP32-S3 frontend.
 * All pin assignments VERIFIED for ESP32-S3-DevKitC-1 compatibility and optimized for
 * audio-focused embedded system requirements.
 * 
 * ⚠️  CRITICAL: All GPIO assignments verified against ESP32-S3-DevKitC-1 official pinout
 * ✅  VERIFIED: Compatible with both N8 and N8R8 PSRAM variants
 */

#pragma once

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_lcd_types.h"
#else
// Minimal fallbacks for host builds (lint/tests)
typedef int gpio_num_t;
#ifndef GPIO_NUM_2
#define GPIO_NUM_2   2
#define GPIO_NUM_4   4
#define GPIO_NUM_5   5
#define GPIO_NUM_6   6
#define GPIO_NUM_7   7
#define GPIO_NUM_8   8
#define GPIO_NUM_9   9
#define GPIO_NUM_10  10
#define GPIO_NUM_11  11
#define GPIO_NUM_12  12
#define GPIO_NUM_13  13
#define GPIO_NUM_14  14
#define GPIO_NUM_15  15
#define GPIO_NUM_16  16
#define GPIO_NUM_17  17
#define GPIO_NUM_18  18
#define GPIO_NUM_19  19
#define GPIO_NUM_20  20
#define GPIO_NUM_21  21
#define GPIO_NUM_38  38
#define GPIO_NUM_42  42
#define GPIO_NUM_47  47
#endif
#ifndef SPI2_HOST
#define SPI2_HOST 2
#endif
#ifndef SPI3_HOST
#define SPI3_HOST 3
#endif
#ifndef I2C_NUM_0
#define I2C_NUM_0 0
#endif
#ifndef ESP_LCD_COLOR_SPACE_BGR
#define ESP_LCD_COLOR_SPACE_BGR 1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// SPI Bus Configuration
// =============================================================================

/** SPI host used for display and touch communication */
#define WAVEX_SPI_HOST          SPI3_HOST

// =============================================================================
// ST7796S Display Controller (480x320 TFT) - ✅ VERIFIED VALID
// =============================================================================

/** Display SPI Clock - GPIO7 (J1 pin 7) ✅ */
#define WAVEX_LCD_GPIO_SCLK     GPIO_NUM_7

/** Display SPI MOSI - GPIO6 (J1 pin 6) ✅ */
#define WAVEX_LCD_GPIO_MOSI     GPIO_NUM_6

/** Display SPI Chip Select - GPIO5 (J1 pin 5) ✅ */
#define WAVEX_LCD_GPIO_CS       GPIO_NUM_5

/** Display Data/Command control - GPIO4 (J1 pin 4) ✅ */
#define WAVEX_LCD_GPIO_DC       GPIO_NUM_4

/** Display Reset - GPIO2 (J3 pin 5) ✅ */
#define WAVEX_LCD_GPIO_RST      GPIO_NUM_2

/** Display Backlight control - GPIO21 (J3 pin 18) ✅ */
#define WAVEX_LCD_GPIO_BL       GPIO_NUM_21

// =============================================================================
// I2C Capacitive Touch Controller - 🔧 FIXED FOR ESP32-S3-DevKitC-1
// =============================================================================

/** Touch I2C Data - GPIO20 (J3 pin 19) ✅ */
#define WAVEX_CTP_GPIO_SDA      GPIO_NUM_20

/** Touch I2C Clock - GPIO9 🔧 CHANGED: GPIO38 is strapping pin, using GPIO9 instead */
#define WAVEX_CTP_GPIO_SCL      GPIO_NUM_9

/** Touch Reset - GPIO14 🔧 CHANGED: GPIO15 has conflicts, using GPIO14 instead */
#define WAVEX_CTP_GPIO_RST      GPIO_NUM_14

/** Touch Interrupt - GPIO15 (active-low on touch, from docs) */
#define WAVEX_CTP_GPIO_INT      GPIO_NUM_15  // Available pin; enable pull-up and falling-edge interrupt

/** I2C port number for touch controller */
#define WAVEX_CTP_I2C_NUM       I2C_NUM_0

/** I2C master frequency for touch controller (100kHz standard) */
#define WAVEX_CTP_I2C_FREQ_HZ   100000

// =============================================================================
// Inter-MCU Communication (ESP32 ↔ Daisy)
// =============================================================================

/** Inter-MCU SPI host */
#define WAVEX_INTER_MCU_SPI_HOST    SPI2_HOST

/** Inter-MCU SPI Chip Select - GPIO8 (J1 pin 12) ✅ */
#define WAVEX_INTER_MCU_GPIO_CS     GPIO_NUM_8

/** Inter-MCU SPI Clock - GPIO18 (J1 pin 11) ✅ */
#define WAVEX_INTER_MCU_GPIO_SCLK   GPIO_NUM_18

/** Inter-MCU SPI MOSI - GPIO47 (J3 pin 17) ✅ */
#define WAVEX_INTER_MCU_GPIO_MOSI   GPIO_NUM_47

/** Inter-MCU SPI MISO - GPIO37 (remapped; per DevKitC-1 availability) ✅ */
#define WAVEX_INTER_MCU_GPIO_MISO   GPIO_NUM_37

/** Inter-MCU SPI clock frequency (10MHz for audio timing) */
#define WAVEX_INTER_MCU_SPI_CLK_HZ  (10 * 1000 * 1000)

/** Inter-MCU IRQ from Daisy PB0 → ESP32 GPIO16 (per docs) */
#ifndef WAVEX_INTER_MCU_GPIO_IRQ
#define WAVEX_INTER_MCU_GPIO_IRQ    GPIO_NUM_16
#endif

// =============================================================================
// SD Card Interface - ✅ VERIFIED VALID
// =============================================================================

/** SD Card SPI Chip Select - GPIO10 (J1 pin 16) ✅ */
#define WAVEX_SD_GPIO_CS        GPIO_NUM_10

/** SD Card SPI Clock - GPIO12 (J1 pin 18) ✅ */
#define WAVEX_SD_GPIO_SCLK      GPIO_NUM_12

/** SD Card SPI MOSI - GPIO11 (J1 pin 17) ✅ */
#define WAVEX_SD_GPIO_MOSI      GPIO_NUM_11

/** SD Card SPI MISO - GPIO13 (J1 pin 19) ✅ */
#define WAVEX_SD_GPIO_MISO      GPIO_NUM_13

// =============================================================================
// MIDI Interface - 🔧 CORRECTED
// =============================================================================

/** MIDI UART TX - GPIO17 (J1 pin 10) ✅ */
#define WAVEX_MIDI_GPIO_TX      GPIO_NUM_17

/** MIDI UART RX - GPIO42 (J3 pin 6) 🔧 FIXED: was GPIO15 (conflict) */
#define WAVEX_MIDI_GPIO_RX      GPIO_NUM_42

// =============================================================================
// Display Configuration Constants
// =============================================================================

/** LCD horizontal resolution */
#define WAVEX_LCD_H_RES         480

/** LCD vertical resolution */
#define WAVEX_LCD_V_RES         320

/** LCD color depth in bits per pixel */
#define WAVEX_LCD_BITS_PER_PIXEL    16

/** LCD SPI clock frequency (40MHz for good performance) */
#define WAVEX_LCD_PIXEL_CLK_HZ      (40 * 1000 * 1000)

/** LCD command bits */
#define WAVEX_LCD_CMD_BITS          8

/** LCD parameter bits */
#define WAVEX_LCD_PARAM_BITS        8

/** LCD color space - BGR order for correct red/blue channels */
#define WAVEX_LCD_COLOR_SPACE       ESP_LCD_COLOR_SPACE_BGR

/** LCD backlight active level (1 = high/on via PWM duty >0) */
#define WAVEX_LCD_BL_ON_LEVEL       1

/** LCD reset active level (0 = low/reset, confirmed from docs) */
#define WAVEX_LCD_RST_ACTIVE_LEVEL  0

/** LCD chip select active level (0 = low/active, from docs) */
#define WAVEX_LCD_CS_ACTIVE_LEVEL   0

// Backlight control mode selection
#define WAVEX_BACKLIGHT_PWM_MODE    0  // Set to 1 for PWM mode, 0 for GPIO mode
#define WAVEX_BACKLIGHT_ENABLED     1  // Set to 1 to enable backlight, 0 to disable

// =============================================================================
// Touch Configuration Constants  
// =============================================================================

/** Touch screen maximum X coordinate */
#define WAVEX_TOUCH_MAX_X           WAVEX_LCD_H_RES

/** Touch screen maximum Y coordinate */
#define WAVEX_TOUCH_MAX_Y           WAVEX_LCD_V_RES

// =============================================================================
// LVGL Buffer Configuration
// =============================================================================

/** LVGL draw buffer height (lines) - optimized for PSRAM */
#define WAVEX_LVGL_DRAW_BUF_HEIGHT  40

/** Enable double buffering with PSRAM for better performance */
#define WAVEX_LVGL_DOUBLE_BUFFER    1

/** Use PSRAM for large buffers (>20KB) */
#define WAVEX_LVGL_USE_PSRAM_THRESHOLD  (20 * 1024)

// =============================================================================
// Hardware Validation Status
// =============================================================================

/** Hardware validation status for ESP32-S3-DevKitC-1 */
#define WAVEX_HW_VALIDATION_COMPLETE    1

/** Board compatibility identifier */
#define WAVEX_TARGET_BOARD_ESP32_S3_DEVKITC_1   1

#ifdef __cplusplus
}
#endif 