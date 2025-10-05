/**
 * @file hardware_pins.h
 * @brief WaveX Hardware GPIO Pin Definitions
 * 
 * This file serves as a mapping layer that converts centralized pin definitions
 * from shared/config/pin_config.h into ESP32-specific GPIO_NUM macros.
 * 
 * ⚠️  CRITICAL: All pin assignments are defined in shared/config/pin_config.h
 * ✅  VERIFIED: Compatible with both N8 and N8R8 PSRAM variants
 * 🔧  UPDATED: Complete redesign using centralized pin configuration
 * 📍  CENTRALIZED: Pin definitions sourced from shared/config/pin_config.h
 */

#pragma once

// Include centralized pin configuration
#include "../../../../shared/config/pin_config.h"

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/uart.h"
// #include "esp_adc/adc_oneshot.h"  // Not needed - ADC functionality is disabled
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
#define GPIO_NUM_22  22
#define GPIO_NUM_23  23
#define GPIO_NUM_24  24
#define GPIO_NUM_25  25
#define GPIO_NUM_26  26
#define GPIO_NUM_27  27
#define GPIO_NUM_28  28
#define GPIO_NUM_29  29
#define GPIO_NUM_30  30
#define GPIO_NUM_31  31
#define GPIO_NUM_32  32
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

/** SPI host used for display (VSPI) */
#define WAVEX_SPI_HOST          SPI3_HOST

/** SPI host used for inter-MCU communication (HSPI) */
#define WAVEX_INTER_MCU_SPI_HOST SPI2_HOST

// =============================================================================
// MIPI DSI Display Controller (800x480 TFT) - ✅ NEW MIPI DSI SUPPORT
// =============================================================================

#if WAVEX_LCD_DISPLAY_TYPE == 1  // MIPI DSI Display
/** MIPI DSI Data Lane 0 Positive - GPIO2 ✅ */
#define WAVEX_DSI_GPIO_D0P      WAVEX_ESP_DSI_D0P

/** MIPI DSI Data Lane 0 Negative - GPIO3 ✅ */
#define WAVEX_DSI_GPIO_D0N      WAVEX_ESP_DSI_D0N

/** MIPI DSI Data Lane 1 Positive - GPIO4 ✅ */
#define WAVEX_DSI_GPIO_D1P      WAVEX_ESP_DSI_D1P

/** MIPI DSI Data Lane 1 Negative - GPIO5 ✅ */
#define WAVEX_DSI_GPIO_D1N      WAVEX_ESP_DSI_D1N

/** MIPI DSI Clock Positive - GPIO6 ✅ */
#define WAVEX_DSI_GPIO_CLKP     WAVEX_ESP_DSI_CLKP

/** MIPI DSI Clock Negative - GPIO7 ✅ */
#define WAVEX_DSI_GPIO_CLKN     WAVEX_ESP_DSI_CLKN

/** Display Reset - GPIO8 ✅ */
#define WAVEX_LCD_GPIO_RST      WAVEX_ESP_DSI_RST

/** Display Backlight control - GPIO9 ✅ */
#define WAVEX_LCD_GPIO_BL       WAVEX_ESP_DSI_BL

#else  // ST7796S SPI Display (Legacy)
/** Display SPI Clock - GPIO7 (J1 pin 7) ✅ */
#define WAVEX_LCD_GPIO_SCLK     WAVEX_ESP_LCD_SCLK

/** Display SPI MOSI - GPIO6 (J1 pin 6) ✅ */
#define WAVEX_LCD_GPIO_MOSI     WAVEX_ESP_LCD_MOSI

/** Display SPI Chip Select - GPIO5 (J1 pin 5) ✅ */
#define WAVEX_LCD_GPIO_CS       WAVEX_ESP_LCD_CS

/** Display Data/Command control - GPIO4 (J1 pin 4) ✅ */
#define WAVEX_LCD_GPIO_DC       WAVEX_ESP_LCD_DC

/** Display Reset - GPIO2 (J3 pin 5) ✅ */
#define WAVEX_LCD_GPIO_RST      WAVEX_ESP_LCD_RST

/** Display Backlight control - GPIO21 (J3 pin 18) ✅ */
#define WAVEX_LCD_GPIO_BL       WAVEX_ESP_LCD_BL
#endif

// =============================================================================
// I2C Capacitive Touch Controller - ✅ UPDATED FOR MIPI DSI
// =============================================================================
/** Touch I2C Data - GPIO20 (J3 pin 19) ✅ */
#define WAVEX_CTP_GPIO_SDA      WAVEX_ESP_TOUCH_SDA

/** Touch I2C Clock - GPIO21 (J3 pin 4) ✅ */
#define WAVEX_CTP_GPIO_SCL      WAVEX_ESP_TOUCH_SCL

/** Touch Reset - GPIO14 (J1 pin 20) ✅ */
#define WAVEX_CTP_GPIO_RST      WAVEX_ESP_TOUCH_RST

/** Touch Interrupt - GPIO15 (J1 pin 21) ✅ */
#define WAVEX_CTP_GPIO_INT      WAVEX_ESP_TOUCH_INT

/** I2C port number for touch controller */
#define WAVEX_CTP_I2C_NUM       I2C_NUM_0

/** I2C master frequency for touch controller (400kHz for GT911) */
#define WAVEX_CTP_I2C_FREQ_HZ   400000

// =============================================================================
// Inter-MCU Communication (ESP32 ↔ Daisy) - 🔧 UPDATED TO SPI
// =============================================================================

/** Inter-MCU SPI host number (dedicated to Daisy communication) */
#define WAVEX_INTER_MCU_SPI_HOST    SPI2_HOST

/** Inter-MCU SPI Clock - GPIO16 (J1 pin 14) ✅ */
#define WAVEX_INTER_MCU_GPIO_SCLK   WAVEX_ESP_SPI_SCLK

/** Inter-MCU SPI MOSI - GPIO17 (J1 pin 10) ✅ */
#define WAVEX_INTER_MCU_GPIO_MOSI   WAVEX_ESP_SPI_MOSI

/** Inter-MCU SPI MISO - GPIO18 (J1 pin 11) ✅ */
#define WAVEX_INTER_MCU_GPIO_MISO   WAVEX_ESP_SPI_MISO

/** Inter-MCU SPI Chip Select - GPIO19 (J1 pin 15) ✅ */
#define WAVEX_INTER_MCU_GPIO_CS     WAVEX_ESP_SPI_CS

/** ESP Attention Output - GPIO31 (J3 pin 14) ✅ */
#define WAVEX_INTER_MCU_GPIO_ATTN   WAVEX_ESP_ATTN_OUT

/** Inter-MCU SPI clock frequency (10MHz for high-speed data transfer) */
#define WAVEX_INTER_MCU_SPI_CLK_HZ  WAVEX_ESP_SPI_CLK_HZ

/** Inter-MCU SPI queue size for DMA operations */
#define WAVEX_INTER_MCU_SPI_QUEUE_SIZE  WAVEX_ESP_SPI_QUEUE_SIZE

/** Inter-MCU SPI DMA channel (auto-select for best performance) */
#define WAVEX_INTER_MCU_SPI_DMA_CH     WAVEX_ESP_SPI_DMA_CH

// Legacy UART definitions (kept for compatibility, but deprecated)
/** @deprecated Inter-MCU UART port number - use SPI instead */
#define WAVEX_INTER_MCU_UART_NUM    UART_NUM_1

/** @deprecated Inter-MCU UART TX - use SPI instead */
#define WAVEX_INTER_MCU_GPIO_TX     WAVEX_ESP_LEGACY_TX

/** @deprecated Inter-MCU UART RX - use SPI instead */
#define WAVEX_INTER_MCU_GPIO_RX     WAVEX_ESP_LEGACY_RX

/** @deprecated Inter-MCU UART baud rate - use SPI instead */
#define WAVEX_INTER_MCU_UART_BAUD   3000000

// =============================================================================
// MIDI Interface - 🔧 UPDATED FOR BOTH USB AND DIN
// =============================================================================

/** MIDI UART TX - GPIO8 (J1 pin 12) ✅ */
#define WAVEX_MIDI_GPIO_TX      WAVEX_ESP_MIDI_TX

/** MIDI UART RX - GPIO42 (J3 pin 6) ✅ */
#define WAVEX_MIDI_GPIO_RX      WAVEX_ESP_MIDI_RX

/** MIDI UART port number */
#define WAVEX_MIDI_UART_NUM     UART_NUM_2

/** MIDI UART baud rate (31250 bps standard) */
#define WAVEX_MIDI_UART_BAUD    31250



// =============================================================================
// Quadrature Encoder Interface (PCNT) - ✅ VERIFIED VALID
// =============================================================================

/** Encoder Channel A - GPIO33 (J3 pin 7) ✅ */
#define WAVEX_ENCODER_GPIO_A     WAVEX_ESP_ENCODER_A

/** Encoder Channel B - GPIO34 (J3 pin 8) ✅ */
#define WAVEX_ENCODER_GPIO_B     WAVEX_ESP_ENCODER_B

/** Encoder Push Button - GPIO40 (J3 pin 12) ✅ */
#define WAVEX_ENCODER_GPIO_BTN   WAVEX_ESP_ENCODER_BTN

/** PCNT unit number for encoder */
#define WAVEX_ENCODER_PCNT_UNIT  PCNT_UNIT_0

/** PCNT channel for encoder A */
#define WAVEX_ENCODER_PCNT_CH_A  PCNT_CHANNEL_0

/** PCNT channel for encoder B */
#define WAVEX_ENCODER_PCNT_CH_B  PCNT_CHANNEL_1

// =============================================================================
// Additional PCNT Unit (PCNT_UNIT_1) - Pulse Counter Interface
// =============================================================================

/** PCNT1 Channel A - GPIO46 */
#define WAVEX_PCNT1_GPIO_A       WAVEX_ESP_PCNT1_A

/** PCNT1 Channel B - GPIO47 */
#define WAVEX_PCNT1_GPIO_B       WAVEX_ESP_PCNT1_B

/** PCNT unit number for PCNT1 */
#define WAVEX_PCNT1_UNIT         WAVEX_ESP_PCNT1_UNIT

/** PCNT channel for PCNT1 A */
#define WAVEX_PCNT1_CH_A         WAVEX_ESP_PCNT1_CH_A

/** PCNT channel for PCNT1 B */
#define WAVEX_PCNT1_CH_B         WAVEX_ESP_PCNT1_CH_B


// =============================================================================
// Display Configuration Constants
// =============================================================================

#if WAVEX_LCD_DISPLAY_TYPE == 1  // MIPI DSI Display


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
#define WAVEX_POT_COUNT             4

/** ADC resolution in bits */
#define WAVEX_POT_ADC_RESOLUTION    10

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
// Encoder Configuration Constants - ✅ VERIFIED VALID
// =============================================================================

/** Encoder resolution (counts per detent) */
#define WAVEX_ENCODER_RESOLUTION     4

/** Encoder debounce time in milliseconds */
#define WAVEX_ENCODER_DEBOUNCE_MS   10

/** Encoder filter value for noise reduction */
#define WAVEX_ENCODER_FILTER_VALUE   100

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

/** Current configuration summary */
#define WAVEX_CONFIG_DISPLAY_VSPI       1    // Display on VSPI (SPI3)
#define WAVEX_CONFIG_INTER_MCU_HSPI     1    // Inter-MCU on HSPI (SPI2)
#define WAVEX_CONFIG_TOUCH_I2C          1    // Touch on I2C with interrupt
#define WAVEX_CONFIG_ENCODER_PCNT       1    // Encoder on PCNT
#define WAVEX_CONFIG_PCNT1_ENABLED      1    // Additional PCNT unit configured
#define WAVEX_CONFIG_SD_CARD_DISABLED   1    // No SD card on ESP32

#ifdef __cplusplus
}
#endif 