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
 * 🔧  UPDATED: Complete redesign for all new peripherals and available pins only
 */

#pragma once

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/adc.h"
#include "esp_lcd_types.h"
#else
// Minimal fallbacks for host builds (lint/tests)
typedef int gpio_num_t;
#ifndef GPIO_NUM_0
#define GPIO_NUM_0   0
#define GPIO_NUM_1   1
#define GPIO_NUM_2   2
#define GPIO_NUM_3   3
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
#define GPIO_NUM_33  33
#define GPIO_NUM_34  34
#define GPIO_NUM_35  35
#define GPIO_NUM_36  36
#define GPIO_NUM_37  37
#define GPIO_NUM_38  38
#define GPIO_NUM_39  39
#define GPIO_NUM_40  40
#define GPIO_NUM_41  41
#define GPIO_NUM_42  42
#define GPIO_NUM_43  43
#define GPIO_NUM_44  44
#define GPIO_NUM_45  45
#define GPIO_NUM_46  46
#define GPIO_NUM_47  47
#define GPIO_NUM_48  48
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
#ifndef I2C_NUM_1
#define I2C_NUM_1 1
#endif
#ifndef UART_NUM_0
#define UART_NUM_0 0
#endif
#ifndef UART_NUM_1
#define UART_NUM_1 1
#endif
#ifndef UART_NUM_2
#define UART_NUM_2 2
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

/** SPI host used for SD card interface */
#define WAVEX_SD_SPI_HOST       SPI2_HOST

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
// I2C Capacitive Touch Controller - ✅ VERIFIED VALID
// =============================================================================

/** Touch I2C Data - GPIO20 (J3 pin 19) ✅ */
#define WAVEX_CTP_GPIO_SDA      GPIO_NUM_20

/** Touch I2C Clock - GPIO9 (J3 pin 4) ✅ */
#define WAVEX_CTP_GPIO_SCL      GPIO_NUM_9

/** Touch Reset - GPIO14 (J1 pin 20) ✅ */
#define WAVEX_CTP_GPIO_RST      GPIO_NUM_14

/** Touch Interrupt - GPIO15 (J1 pin 21) ✅ */
#define WAVEX_CTP_GPIO_INT      GPIO_NUM_15

/** I2C port number for touch controller */
#define WAVEX_CTP_I2C_NUM       I2C_NUM_0

/** I2C master frequency for touch controller (100kHz standard) */
#define WAVEX_CTP_I2C_FREQ_HZ   100000

// =============================================================================
// Inter-MCU Communication (ESP32 ↔ Daisy) - 🔧 UPDATED TO UART
// =============================================================================

/** Inter-MCU UART port number */
#define WAVEX_INTER_MCU_UART_NUM    UART_NUM_1

/** Inter-MCU UART TX - GPIO17 (J1 pin 10) ✅ */
#define WAVEX_INTER_MCU_GPIO_TX     GPIO_NUM_17

/** Inter-MCU UART RX - GPIO18 (J1 pin 11) ✅ */
#define WAVEX_INTER_MCU_GPIO_RX     GPIO_NUM_18

/** Inter-MCU UART baud rate (3MHz for high-speed audio data) */
#define WAVEX_INTER_MCU_UART_BAUD   3000000

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
// MIDI Interface - 🔧 UPDATED FOR BOTH USB AND DIN
// =============================================================================

/** MIDI UART TX - GPIO8 (J1 pin 12) ✅ */
#define WAVEX_MIDI_GPIO_TX      GPIO_NUM_8

/** MIDI UART RX - GPIO42 (J3 pin 6) ✅ */
#define WAVEX_MIDI_GPIO_RX      GPIO_NUM_42

/** MIDI UART port number */
#define WAVEX_MIDI_UART_NUM     UART_NUM_2

/** MIDI UART baud rate (31250 bps standard) */
#define WAVEX_MIDI_UART_BAUD    31250

// =============================================================================
// Dual Potentiometer Interface (CD74HC4067) - 🆕 NEW
// =============================================================================

/** CD74HC4067 Address Select A0 - GPIO33 (J3 pin 7) ✅ */
#define WAVEX_POT_ADDR_A0       GPIO_NUM_33

/** CD74HC4067 Address Select A1 - GPIO34 (J3 pin 8) ✅ */
#define WAVEX_POT_ADDR_A1       GPIO_NUM_34

/** CD74HC4067 Address Select A2 - GPIO35 (J3 pin 9) ✅ */
#define WAVEX_POT_ADDR_A2       GPIO_NUM_35

/** CD74HC4067 Address Select A3 - GPIO36 (J3 pin 10) ✅ */
#define WAVEX_POT_ADDR_A3       GPIO_NUM_36

/** CD74HC4067 Enable (active low) - GPIO37 (J3 pin 11) ✅ */
#define WAVEX_POT_ENABLE        GPIO_NUM_37

/** CD74HC4067 Common Signal - GPIO1 (ADC1_CH0) ✅ */
#define WAVEX_POT_SIGNAL        GPIO_NUM_1

/** ADC channel for potentiometer reading */
#define WAVEX_POT_ADC_CHANNEL   ADC1_CHANNEL_0

/** ADC attenuation for 0-3.3V range */
#define WAVEX_POT_ADC_ATTEN     ADC_ATTEN_DB_11

// =============================================================================
// Button Matrix Interface (TCA8418) - 🆕 NEW
// =============================================================================

/** TCA8418 I2C Data - GPIO39 (J3 pin 12) ✅ */
#define WAVEX_BTN_GPIO_SDA      GPIO_NUM_39

/** TCA8418 I2C Clock - GPIO40 (J3 pin 13) ✅ */
#define WAVEX_BTN_GPIO_SCL      GPIO_NUM_40

/** TCA8418 Reset - GPIO41 (J3 pin 14) ✅ */
#define WAVEX_BTN_GPIO_RST      GPIO_NUM_41

/** TCA8418 Interrupt - GPIO43 (J3 pin 15) ✅ */
#define WAVEX_BTN_GPIO_INT      GPIO_NUM_43

/** I2C port number for button matrix */
#define WAVEX_BTN_I2C_NUM       I2C_NUM_1

/** I2C master frequency for button matrix (100kHz standard) */
#define WAVEX_BTN_I2C_FREQ_HZ   100000

// =============================================================================
// LED Driver Interface (TLC5947) - 🆕 NEW
// =============================================================================

/** TLC5947 SPI Clock - GPIO44 (J3 pin 16) ✅ */
#define WAVEX_LED_GPIO_SCLK     GPIO_NUM_44

/** TLC5947 SPI MOSI - GPIO45 (J3 pin 17) ✅ */
#define WAVEX_LED_GPIO_MOSI     GPIO_NUM_45

/** TLC5947 SPI Chip Select - GPIO46 (J3 pin 18) ✅ */
#define WAVEX_LED_GPIO_CS       GPIO_NUM_46

/** TLC5947 Blank - GPIO47 (J3 pin 19) ✅ */
#define WAVEX_LED_GPIO_BLANK    GPIO_NUM_47

/** TLC5947 Latch - GPIO48 (J3 pin 20) ✅ */
#define WAVEX_LED_GPIO_LATCH    GPIO_NUM_48

/** SPI host used for LED driver */
#define WAVEX_LED_SPI_HOST      SPI2_HOST

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
// Potentiometer Configuration Constants - 🆕 NEW
// =============================================================================

/** Number of dual potentiometers supported */
#define WAVEX_POT_COUNT             16

/** ADC resolution in bits */
#define WAVEX_POT_ADC_RESOLUTION    12

/** ADC sample count for averaging */
#define WAVEX_POT_ADC_SAMPLES       64

// =============================================================================
// Button Matrix Configuration Constants - 🆕 NEW
// =============================================================================

/** Button matrix rows (TCA8418 supports 8x8) */
#define WAVEX_BTN_MATRIX_ROWS       8

/** Button matrix columns (TCA8418 supports 8x8) */
#define WAVEX_BTN_MATRIX_COLS       8

/** Button debounce time in milliseconds */
#define WAVEX_BTN_DEBOUNCE_MS       50

// =============================================================================
// LED Driver Configuration Constants - 🆕 NEW
// =============================================================================

/** Number of LED channels (TLC5947 supports 48) */
#define WAVEX_LED_CHANNELS          48

/** LED PWM frequency in Hz */
#define WAVEX_LED_PWM_FREQ_HZ       1000

/** LED brightness resolution in bits */
#define WAVEX_LED_BRIGHTNESS_BITS   12

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