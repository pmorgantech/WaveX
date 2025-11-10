/**
 * @file hardware_config.h
 * @brief WaveX Hardware Component Configuration
 *
 * This file defines ALL hardware component enable/disable macros and configuration
 * options for the WaveX project. This is the single source of truth for all
 * hardware feature flags.
 *
 * Components can be completely disabled at compile time by setting their
 * respective _ENABLED macro to 0, or runtime controlled via additional
 * configuration options.
 */

#pragma once

// ============================================================================
// SHARED COMPONENTS (Both Daisy and ESP32)
// ============================================================================

// Inter-MCU Communication Link
#ifndef WAVEX_INTER_MCU_LINK_ENABLED
#define WAVEX_INTER_MCU_LINK_ENABLED 1
#endif

// ============================================================================
// DAISY-SPECIFIC COMPONENTS
// ============================================================================

// Audio Engine (Daisy only)
#ifndef WAVEX_AUDIO_ENGINE_ENABLED
#define WAVEX_AUDIO_ENGINE_ENABLED 1
#endif

// DAC CV Outputs (Daisy only)
#ifndef WAVEX_DAISY_CV_OUTPUTS_ENABLED
#define WAVEX_DAISY_CV_OUTPUTS_ENABLED 0
#endif

// USB Configuration (Daisy only)
#ifndef WAVEX_DAISY_USB_ENABLED
#define WAVEX_DAISY_USB_ENABLED 1
#endif

// SD Card (Daisy only)
#ifndef WAVEX_DAISY_SD_CARD_ENABLED
#define WAVEX_DAISY_SD_CARD_ENABLED 1
#endif

// Daisy SD Card Backend Selection
// 0 = SPI-based SD (legacy), 1 = SDMMC (SDIO) 4-bit mode using libDaisy
#ifndef WAVEX_DAISY_SD_CARD_BACKEND
#define WAVEX_DAISY_SD_CARD_BACKEND 1
#endif

// Ensure SPI SD backend is completely disabled when using SDMMC
#if WAVEX_DAISY_SD_CARD_BACKEND == 1
#define DISABLE_SD_SPI_BACKEND 0
#endif

// SD Card debug logging (Daisy only)
#ifndef WAVEX_DAISY_SD_DEBUG
#define WAVEX_DAISY_SD_DEBUG 0
#endif

// SD Card detect pin (Daisy only) - set to -1 to disable card detect
#ifndef WAVEX_DAISY_SD_CARD_DETECT_PIN
#define WAVEX_DAISY_SD_CARD_DETECT_PIN 15
#endif

// SD Card bus width (Daisy only) - 1 for 1-bit mode, 4 for 4-bit mode
#ifndef WAVEX_DAISY_SD_CARD_BUS_WIDTH
#define WAVEX_DAISY_SD_CARD_BUS_WIDTH 4
#endif

/**
 * @def WAVEX_DAISY_SD_CARD_SPEED
 * @brief Sets the speed of the SD card interface.
 *
 * This macro configures the speed of the SD card communication. The available
 * options are:
 * - 0: SLOW (~400 KHz) - Recommended for maximum compatibility and debugging.
 * - 1: MEDIUM_SLOW (~11 MHz)
 * - 2: STANDARD (~20 MHz)
 * - 3: FAST (~40 MHz)
 *
 * The default value is 2 (STANDARD). A lower speed might be necessary if you
 * encounter data corruption or initialization issues, especially with long
 * cables or certain SD card models.
 *   @note Speed settings correspond to SdmmcHandler::Speed enum
 *   0: SLOW (~400 KHz), 1: MEDIUM_SLOW, 2: STANDARD (~12.5 MHz), 3: FAST (~25 MHz)
 */
#ifndef WAVEX_DAISY_SD_CARD_SPEED
#define WAVEX_DAISY_SD_CARD_SPEED 2
#endif

// External Flash (Daisy only)
#ifndef WAVEX_DAISY_EXTERNAL_FLASH_ENABLED
#define WAVEX_DAISY_EXTERNAL_FLASH_ENABLED 1
#endif

// PCM1690 TDM DAC (Daisy only)
#ifndef WAVEX_DAISY_TDM_DAC_ENABLED
#define WAVEX_DAISY_TDM_DAC_ENABLED 1
#endif

// Loop/CPU probe (Daisy only)
#ifndef WAVEX_DAISY_LOOP_PROBE_ENABLED
#define WAVEX_DAISY_LOOP_PROBE_ENABLED 0
#endif

#ifndef WAVEX_DAISY_LOOP_PROBE_PIN
#define WAVEX_DAISY_LOOP_PROBE_PIN 15
#endif

// ============================================================================
// ESP32-SPECIFIC COMPONENTS
// ============================================================================

// Quadrature Encoder (PCNT peripheral)
#ifndef WAVEX_ESP_ENCODER_PCNT_ENABLED
#define WAVEX_ESP_ENCODER_PCNT_ENABLED 1
#endif

// Additional PCNT Unit (PCNT_UNIT_1)
#ifndef WAVEX_ESP_PCNT1_ENABLED
#define WAVEX_ESP_PCNT1_ENABLED 1
#endif

// CD74HC4067 16-channel Analog Multiplexer
#ifndef WAVEX_ESP_MUX_ENABLED
#define WAVEX_ESP_MUX_ENABLED 0  // Currently disabled, using encoder instead
#endif

// TCA8418 8x8 Capacitive Button Matrix
#ifndef WAVEX_ESP_BUTTON_MATRIX_ENABLED
#define WAVEX_ESP_BUTTON_MATRIX_ENABLED 1  // Enabled for switch/button inputs
#endif

// MIPI DSI LCD Display (5-DSI-TOUCH-A)
#ifndef WAVEX_LCD_DISPLAY_ENABLED
#define WAVEX_LCD_DISPLAY_ENABLED 1
#endif

// MIPI DSI Display Type Selection
#ifndef WAVEX_LCD_DISPLAY_TYPE
#define WAVEX_LCD_DISPLAY_TYPE 1  // 0 = ST7796S SPI, 1 = MIPI DSI
#endif

// USB MIDI Interface
#ifndef WAVEX_ESP_USB_MIDI_ENABLED
#define WAVEX_ESP_USB_MIDI_ENABLED 1
#endif

// PSRAM (ESP32 only)
#ifndef WAVEX_ESP_PSRAM_ENABLED
#define WAVEX_ESP_PSRAM_ENABLED 1
#endif

// WiFi (ESP32 only)
#ifndef WAVEX_ESP_WIFI_ENABLED
#define WAVEX_ESP_WIFI_ENABLED 0
#endif

// Bluetooth (ESP32 only)
#ifndef WAVEX_ESP_BLUETOOTH_ENABLED
#define WAVEX_ESP_BLUETOOTH_ENABLED 0
#endif

// ============================================================================
// COMPONENT-SPECIFIC CONFIGURATION OPTIONS
// ============================================================================

// Audio Engine Configuration
#if WAVEX_AUDIO_ENGINE_ENABLED
// Set to 1 to enable audio output, 0 to mute (write zeros to output)
#ifndef WAVEX_AUDIO_OUTPUT_ENABLED
#define WAVEX_AUDIO_OUTPUT_ENABLED 1
#endif

// Set to 1 to enable audio input processing, 0 to ignore input
#ifndef WAVEX_AUDIO_INPUT_ENABLED
#define WAVEX_AUDIO_INPUT_ENABLED 1
#endif

// Audio sample rate configuration
#ifndef WAVEX_AUDIO_SAMPLE_RATE
#define WAVEX_AUDIO_SAMPLE_RATE 44100
#endif

// Audio block size configuration
#ifndef WAVEX_AUDIO_BLOCK_SIZE
#define WAVEX_AUDIO_BLOCK_SIZE 48
#endif

// Audio buffer configuration
#ifndef WAVEX_AUDIO_BUFFER_SIZE
#define WAVEX_AUDIO_BUFFER_SIZE 256
#endif

// Audio processing priority
#ifndef WAVEX_AUDIO_PRIORITY
#define WAVEX_AUDIO_PRIORITY 0
#endif

// Interval for sending meter updates over SPI (ms)
#ifndef WAVEX_AUDIO_METERS_SEND_INTERVAL_MS
#define WAVEX_AUDIO_METERS_SEND_INTERVAL_MS 50
#endif
#endif

// DAC CV Outputs Configuration
#if WAVEX_DAISY_CV_OUTPUTS_ENABLED
// DAC resolution configuration
#ifndef WAVEX_DAC_RESOLUTION
#define WAVEX_DAC_RESOLUTION 12
#endif

// DAC reference voltage
#ifndef WAVEX_DAC_REFERENCE_VOLTAGE
#define WAVEX_DAC_REFERENCE_VOLTAGE 3.3f
#endif

// DAC output range
#ifndef WAVEX_DAC_OUTPUT_MIN
#define WAVEX_DAC_OUTPUT_MIN 0.0f
#endif

#ifndef WAVEX_DAC_OUTPUT_MAX
#define WAVEX_DAC_OUTPUT_MAX 3.3f
#endif

// DAC update rate (Hz)
#ifndef WAVEX_DAC_UPDATE_RATE
#define WAVEX_DAC_UPDATE_RATE 1000
#endif
#endif

// PCM1690 TDM DAC Configuration
#if WAVEX_DAISY_TDM_DAC_ENABLED
// TDM configuration
#ifndef WAVEX_PCM1690_TDM_CHANNELS
#define WAVEX_PCM1690_TDM_CHANNELS 8
#endif

#ifndef WAVEX_PCM1690_TDM_SLOT_WIDTH
#define WAVEX_PCM1690_TDM_SLOT_WIDTH 32
#endif

#ifndef WAVEX_PCM1690_TDM_FRAME_SYNC_POLARITY
#define WAVEX_PCM1690_TDM_FRAME_SYNC_POLARITY 0  // 0 = active low, 1 = active high
#endif

#ifndef WAVEX_PCM1690_TDM_BIT_CLOCK_POLARITY
#define WAVEX_PCM1690_TDM_BIT_CLOCK_POLARITY 0  // 0 = rising edge, 1 = falling edge
#endif

// Audio format configuration
#ifndef WAVEX_PCM1690_AUDIO_FORMAT
#define WAVEX_PCM1690_AUDIO_FORMAT 0  // 0 = I2S, 1 = Left-justified, 2 = Right-justified
#endif

#ifndef WAVEX_PCM1690_AUDIO_RESOLUTION
#define WAVEX_PCM1690_AUDIO_RESOLUTION 24  // 16, 20, or 24 bit
#endif

// Volume control
#ifndef WAVEX_PCM1690_VOLUME_CONTROL_ENABLED
#define WAVEX_PCM1690_VOLUME_CONTROL_ENABLED 1
#endif

#ifndef WAVEX_PCM1690_DEFAULT_VOLUME
#define WAVEX_PCM1690_DEFAULT_VOLUME 0  // 0 = 0dB, -127 = -127dB
#endif

// Power management
#ifndef WAVEX_PCM1690_POWER_DOWN_ENABLED
#define WAVEX_PCM1690_POWER_DOWN_ENABLED 1
#endif

// Mute control
#ifndef WAVEX_PCM1690_MUTE_ENABLED
#define WAVEX_PCM1690_MUTE_ENABLED 1
#endif

#ifndef WAVEX_PCM1690_DEFAULT_MUTE_STATE
#define WAVEX_PCM1690_DEFAULT_MUTE_STATE 0  // 0 = unmuted, 1 = muted
#endif
#endif

// LCD Display Configuration
#if WAVEX_LCD_DISPLAY_ENABLED
// Set to 1 to enable backlight control, 0 to keep backlight always on
#ifndef WAVEX_LCD_BACKLIGHT_CONTROL_ENABLED
#define WAVEX_LCD_BACKLIGHT_CONTROL_ENABLED 1
#endif

// Set to 1 to enable touch input processing, 0 to disable touch
#ifndef WAVEX_LCD_TOUCH_ENABLED
#define WAVEX_LCD_TOUCH_ENABLED 1
#endif

// MIPI DSI Display Resolution (5-DSI-TOUCH-A)
#ifndef WAVEX_LCD_WIDTH
#define WAVEX_LCD_WIDTH 1280
#endif

#ifndef WAVEX_LCD_HEIGHT
#define WAVEX_LCD_HEIGHT 720
#endif

#ifndef WAVEX_LCD_COLOR_DEPTH
#define WAVEX_LCD_COLOR_DEPTH 16
#endif

// Touch coordinate limits (defaults to LCD resolution)
#ifndef WAVEX_TOUCH_MAX_X
#define WAVEX_TOUCH_MAX_X WAVEX_LCD_WIDTH
#endif

#ifndef WAVEX_TOUCH_MAX_Y
#define WAVEX_TOUCH_MAX_Y WAVEX_LCD_HEIGHT
#endif

// LVGL buffer configuration
#ifndef WAVEX_LVGL_DRAW_BUF_HEIGHT
#define WAVEX_LVGL_DRAW_BUF_HEIGHT 40
#endif

#ifndef WAVEX_LVGL_DOUBLE_BUFFER
#define WAVEX_LVGL_DOUBLE_BUFFER 1
#endif

#ifndef WAVEX_LVGL_USE_PSRAM_THRESHOLD
#define WAVEX_LVGL_USE_PSRAM_THRESHOLD (20 * 1024)
#endif

// MIPI DSI Configuration
#ifndef WAVEX_DSI_LANE_BITRATE_MBPS
#define WAVEX_DSI_LANE_BITRATE_MBPS 1500  // Default bitrate
#endif

#ifndef WAVEX_DSI_COLOR_FORMAT
#define WAVEX_DSI_COLOR_FORMAT 0  // 0 = RGB565, 1 = RGB888
#endif

#endif  // WAVEX_LCD_DISPLAY_ENABLED

// Encoder Configuration
#if WAVEX_ESP_ENCODER_PCNT_ENABLED
// Set to 1 to enable encoder input processing, 0 to disable
#ifndef WAVEX_ENCODER_INPUT_ENABLED
#define WAVEX_ENCODER_INPUT_ENABLED 1
#endif

// Set to 1 to enable encoder interrupt handling, 0 for polling only
#ifndef WAVEX_ENCODER_IRQ_ENABLED
#define WAVEX_ENCODER_IRQ_ENABLED 1
#endif

// PCNT unit selection
#ifndef WAVEX_ENCODER_PCNT_UNIT
#define WAVEX_ENCODER_PCNT_UNIT PCNT_UNIT_0
#endif

// PCNT channel configuration
#ifndef WAVEX_ENCODER_PCNT_CH_A
#define WAVEX_ENCODER_PCNT_CH_A PCNT_CHANNEL_0
#endif

#ifndef WAVEX_ENCODER_PCNT_CH_B
#define WAVEX_ENCODER_PCNT_CH_B PCNT_CHANNEL_1
#endif

// Encoder filter configuration
#ifndef WAVEX_ENCODER_FILTER_ENABLED
#define WAVEX_ENCODER_FILTER_ENABLED 1
#endif

#ifndef WAVEX_ENCODER_FILTER_VALUE
#define WAVEX_ENCODER_FILTER_VALUE 100
#endif
#endif

// PCNT1 Configuration
#if WAVEX_ESP_PCNT1_ENABLED
// PCNT unit selection for PCNT1
#ifndef WAVEX_PCNT1_UNIT
#define WAVEX_PCNT1_UNIT PCNT_UNIT_1
#endif

// PCNT channel configuration for PCNT1
#ifndef WAVEX_PCNT1_CH_A
#define WAVEX_PCNT1_CH_A PCNT_CHANNEL_0
#endif

#ifndef WAVEX_PCNT1_CH_B
#define WAVEX_PCNT1_CH_B PCNT_CHANNEL_1
#endif

// PCNT1 filter configuration
#ifndef WAVEX_PCNT1_FILTER_ENABLED
#define WAVEX_PCNT1_FILTER_ENABLED 1
#endif

#ifndef WAVEX_PCNT1_FILTER_VALUE
#define WAVEX_PCNT1_FILTER_VALUE 100
#endif

// PCNT1 interrupt thresholds
#ifndef WAVEX_PCNT1_THRESH_POS
#define WAVEX_PCNT1_THRESH_POS 4
#endif

#ifndef WAVEX_PCNT1_THRESH_NEG
#define WAVEX_PCNT1_THRESH_NEG -4
#endif
#endif
// Optional LED driver configuration (e.g., TLC5947)
#ifndef WAVEX_LED_CHANNELS
#define WAVEX_LED_CHANNELS 48
#endif

#ifndef WAVEX_LED_PWM_FREQ_HZ
#define WAVEX_LED_PWM_FREQ_HZ 1000
#endif

#ifndef WAVEX_LED_BRIGHTNESS_BITS
#define WAVEX_LED_BRIGHTNESS_BITS 12
#endif

// Optional potentiometer configuration (e.g., MCP3008 via SPI)
#ifndef WAVEX_POT_COUNT
#define WAVEX_POT_COUNT 4
#endif

#ifndef WAVEX_POT_ADC_RESOLUTION
#define WAVEX_POT_ADC_RESOLUTION 10
#endif

#ifndef WAVEX_POT_ADC_SAMPLES
#define WAVEX_POT_ADC_SAMPLES 64
#endif

// Rotary Encoder Configuration (via MCP3008 ADC)
#ifndef WAVEX_ROTARY_ENCODER_COUNT
#define WAVEX_ROTARY_ENCODER_COUNT 4  // 4x dual rotary encoders
#endif

#ifndef WAVEX_ROTARY_ENCODER_TYPE
#define WAVEX_ROTARY_ENCODER_TYPE 1  // 1 = Endless rotary via ADC
#endif

// Optional button matrix configuration (e.g., TCA8418)
#ifndef WAVEX_BTN_MATRIX_ROWS
#define WAVEX_BTN_MATRIX_ROWS 8
#endif

#ifndef WAVEX_BTN_MATRIX_COLS
#define WAVEX_BTN_MATRIX_COLS 8
#endif

#ifndef WAVEX_BTN_DEBOUNCE_MS
#define WAVEX_BTN_DEBOUNCE_MS 50
#endif

// USB MIDI Configuration
#if WAVEX_ESP_USB_MIDI_ENABLED
// Set to 1 to enable USB MIDI input, 0 to disable
#ifndef WAVEX_USB_MIDI_INPUT_ENABLED
#define WAVEX_USB_MIDI_INPUT_ENABLED 1
#endif

// Set to 1 to enable USB MIDI output, 0 to disable
#ifndef WAVEX_USB_MIDI_OUTPUT_ENABLED
#define WAVEX_USB_MIDI_OUTPUT_ENABLED 1
#endif

// USB MIDI buffer sizes
#ifndef WAVEX_USB_MIDI_RX_BUFFER_SIZE
#define WAVEX_USB_MIDI_RX_BUFFER_SIZE 64
#endif

#ifndef WAVEX_USB_MIDI_TX_BUFFER_SIZE
#define WAVEX_USB_MIDI_TX_BUFFER_SIZE 64
#endif

// USB MIDI task priorities
#ifndef WAVEX_USB_MIDI_TASK_PRIORITY
#define WAVEX_USB_MIDI_TASK_PRIORITY 5
#endif

#ifndef WAVEX_USB_MIDI_TASK_STACK_SIZE
#define WAVEX_USB_MIDI_TASK_STACK_SIZE 4096
#endif
#endif

// 4067 Mux Configuration
#if WAVEX_ESP_MUX_ENABLED
// ADC configuration for mux
#ifndef WAVEX_4067_ADC_UNIT
#define WAVEX_4067_ADC_UNIT ADC_UNIT_1
#endif

#ifndef WAVEX_4067_ADC_CHANNEL
#define WAVEX_4067_ADC_CHANNEL ADC_CHANNEL_0
#endif

// Mux address pins configuration
#ifndef WAVEX_4067_ADDR_PINS
#define WAVEX_4067_ADDR_PINS {33, 34, 35, 36}
#endif

#ifndef WAVEX_4067_ENABLE_PIN
#define WAVEX_4067_ENABLE_PIN 37
#endif
#endif

// TCA8418 Button Matrix Configuration
#if WAVEX_ESP_BUTTON_MATRIX_ENABLED
// I2C configuration
#ifndef WAVEX_TCA8418_I2C_PORT
#define WAVEX_TCA8418_I2C_PORT I2C_NUM_0
#endif

#ifndef WAVEX_TCA8418_I2C_CLOCK_SPEED
#define WAVEX_TCA8418_I2C_CLOCK_SPEED (400 * 1000)  // 400 kHz
#endif

// Button matrix dimensions
#ifndef WAVEX_TCA8418_ROWS
#define WAVEX_TCA8418_ROWS 8
#endif

#ifndef WAVEX_TCA8418_COLUMNS
#define WAVEX_TCA8418_COLUMNS 8
#endif

// Task configuration
#ifndef WAVEX_TCA8418_TASK_PRIORITY
#define WAVEX_TCA8418_TASK_PRIORITY 4
#endif

#ifndef WAVEX_TCA8418_TASK_STACK_SIZE
#define WAVEX_TCA8418_TASK_STACK_SIZE 2048
#endif
#endif

// ============================================================================
// DEPENDENCY CHECKS
// ============================================================================

// Ensure inter-MCU link is enabled if any component that depends on it is enabled
#if (WAVEX_AUDIO_ENGINE_ENABLED || WAVEX_DAC_CV_OUTPUTS_ENABLED || WAVEX_ENCODER_PCNT_ENABLED || \
     WAVEX_PCNT1_ENABLED || WAVEX_4067_MUX_ENABLED || WAVEX_TCA8418_BUTTON_MATRIX_ENABLED ||     \
     WAVEX_LCD_DISPLAY_ENABLED || WAVEX_USB_MIDI_ENABLED) &&                                     \
    !WAVEX_INTER_MCU_LINK_ENABLED
#error "Inter-MCU link must be enabled when using components that depend on it"
#endif
