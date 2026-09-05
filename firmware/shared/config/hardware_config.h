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

// Digital voice count (Daisy only). A measured DTCM/CPU budget, not a design
// choice (features/track-and-patch-model.md §5): raise it only after a DWT
// callback-cycle measurement at the new count and a look at the linker
// report. Independent of the analog Stage B voice count (8 PCM1690 TDM
// slots, 8 CV calibration groups) and of the mixer's kMaxMixChannels.
#ifndef WAVEX_NUM_VOICES
#define WAVEX_NUM_VOICES 8
#endif

// Sample Pool capacity (Daisy only): how many samples can be resident at
// once, whoever loaded them (track-and-patch-model.md §4, decided
// 2026-09-04). The registry indexes by id - never scans - so this is a
// table-size decision (records live in SDRAM), not a per-note cost. Audio
// memory is the real cap: 1024 samples average under 60 KB each. Admission
// beyond it fails with a reason; nothing is evicted.
#ifndef WAVEX_SAMPLE_POOL_CAPACITY
#define WAVEX_SAMPLE_POOL_CAPACITY 1024
#endif

// DAC CV Outputs (Daisy only)
#ifndef WAVEX_DAISY_CV_OUTPUTS_ENABLED
#define WAVEX_DAISY_CV_OUTPUTS_ENABLED 0
#endif

// Voice output backend (Daisy only) - architecture.md §5.3.
// Stage A: all voices sum to the SAI1 stereo codec (StereoMixSink).
// Stage B: each voice gets a dedicated PCM1690 TDM slot on SAI2 (TdmVoiceSink,
// stub until Phase 3 SAI2/PCM1690 bring-up).
#ifndef WAVEX_VOICE_OUTPUT_STEREO_MIX
#define WAVEX_VOICE_OUTPUT_STEREO_MIX 0
#endif
#ifndef WAVEX_VOICE_OUTPUT_TDM8
#define WAVEX_VOICE_OUTPUT_TDM8 1
#endif
#ifndef WAVEX_VOICE_OUTPUT_BACKEND
#define WAVEX_VOICE_OUTPUT_BACKEND WAVEX_VOICE_OUTPUT_STEREO_MIX
#endif

// Master switch for the analog CV path (Daisy only).
//
// OFF by default as of 2026-09-02. WaveX's filter and VCA are DIGITAL: each
// voice owns a state-variable filter (audio/svf_filter.hpp) and its own gain,
// which is what MSG_CONTROL_CHANGE actually drives and what you hear. The
// analog path below is the earlier plan - a single MCP4728 quad-DAC emitting
// one shared CV frame for an external paraphonic SSI2164 VCF/VCA - and no such
// board is fitted.
//
// It stayed switched on long after the digital filter replaced it, so every
// 1 kHz control tick was computing a paraphonic envelope and a CvShapeCutoff()
// (two expf() calls) for hardware that is not there, and the main loop was
// flushing I2C to a DAC that never answers. One knob drove both paths.
//
// Everything is retained rather than deleted, and both flag sets still build,
// so re-enabling this is a flag rather than an archaeology exercise. Turn it
// on together with a real CV backend and the calibration page.
#ifndef WAVEX_ANALOG_CV_ENABLED
#define WAVEX_ANALOG_CV_ENABLED 0
#endif

// CV backend (Daisy only) - architecture.md §5.3.
// Stage A: single MCP4728 I2C quad-DAC serving WAVEX_ANALOG_CV_GROUPS=1
// (paraphonic, shared VCF/VCA). Stage B: MCP48CMB28 SPI chain serving
// WAVEX_ANALOG_CV_GROUPS=8 (one physical CV group per voice).
#ifndef WAVEX_CV_BACKEND_MCP4728
#define WAVEX_CV_BACKEND_MCP4728 0
#endif
#ifndef WAVEX_CV_BACKEND_MCP48
#define WAVEX_CV_BACKEND_MCP48 1
#endif
#ifndef WAVEX_CV_BACKEND
#define WAVEX_CV_BACKEND WAVEX_CV_BACKEND_MCP4728
#endif

// Physical analog CV groups the CV group router folds voice-indexed targets
// onto (1 = paraphonic Stage A, 8 = full voice board Stage B).
#ifndef WAVEX_ANALOG_CV_GROUPS
#define WAVEX_ANALOG_CV_GROUPS 1
#endif

// Calibration tables are always sized for the maximum voice count regardless
// of WAVEX_ANALOG_CV_GROUPS, so the Stage A -> B transition never touches
// stored calibration data.
#ifndef WAVEX_ANALOG_CV_GROUPS_MAX
#define WAVEX_ANALOG_CV_GROUPS_MAX 8
#endif

// USB Configuration (Daisy only)
#ifndef WAVEX_DAISY_USB_ENABLED
#define WAVEX_DAISY_USB_ENABLED 1
#endif

// Wait for a USB-CDC serial terminal at boot (Daisy only). libDaisy's
// StartLog(true) puts the logger in synchronous mode: every PrintLine
// busy-waits until a USB host accepts the transfer, and the first log line
// inside StartLog itself never returns without one - a unit powered
// standalone (no PC) hangs before the audio engine starts (review H8).
// Default 0 = boot unconditionally, early boot logs are dropped if no
// terminal is attached. Set to 1 on the bench when catching boot logs
// matters more than standalone operation.
#ifndef WAVEX_DAISY_WAIT_FOR_SERIAL
#define WAVEX_DAISY_WAIT_FOR_SERIAL 0
#endif

// SD Card (Daisy only)
#ifndef WAVEX_DAISY_SD_CARD_ENABLED
#define WAVEX_DAISY_SD_CARD_ENABLED 1
#endif

// SFZ boot import (docs/features/sfz-import.md). Now OFF by default: it
// loads into instrument slot 0 before audio starts, so on any card holding
// the conventional file Track 1 came up owned by a Patch - and a Track
// holding a Patch refuses a bare-sample Select for the rest of the session.
// That is what made "Select does nothing on Track 1" look like a broken
// button rather than an occupied Track (2026-09-03 bench session).
//
// Loading a Patch is now a browser action that asks which Track to use, so
// nothing claims a Track without being asked. Set this to 1 to restore the
// old boot-time behaviour; a missing file remains non-fatal either way.
#ifndef WAVEX_DAISY_SFZ_BOOT_ENABLED
#define WAVEX_DAISY_SFZ_BOOT_ENABLED 0
#endif
#ifndef WAVEX_DAISY_SFZ_BOOT_PATH
#define WAVEX_DAISY_SFZ_BOOT_PATH "0:/wavex/sfz/default/default.sfz"
#endif

// Resident-instrument admission limits. Keep 8 MiB free for browser loads and
// later runtime work; streamed SFZ zones are deliberately out of v1 scope.
#ifndef WAVEX_INST_LOAD_RESERVE_BYTES
#define WAVEX_INST_LOAD_RESERVE_BYTES (8u * 1024u * 1024u)
#endif
#ifndef WAVEX_INST_MAX_RAM_SAMPLE_BYTES
#define WAVEX_INST_MAX_RAM_SAMPLE_BYTES (4u * 1024u * 1024u)
#endif

// Daisy SD Card Backend Selection
// 0 = SPI-based SD (legacy), 1 = SDMMC (SDIO) 4-bit mode using libDaisy
#ifndef WAVEX_DAISY_SD_CARD_BACKEND
#define WAVEX_DAISY_SD_CARD_BACKEND 1
#endif

// Vestigial: nothing in the tree consumes DISABLE_SD_SPI_BACKEND, and
// defining it to 0 disables nothing (the comment previously here claimed it
// switched the SPI SD backend off under SDMMC - it never did).
#if WAVEX_DAISY_SD_CARD_BACKEND == 1
#define DISABLE_SD_SPI_BACKEND 0
#endif

/**
 * @def WAVEX_DAISY_SD_AUTO_FORMAT
 * @brief Whether an unreadable card may be FORMATTED automatically.
 *
 * Default 0, and it should stay there on any board a user puts their own card
 * into. FatFS reports FR_NO_FILESYSTEM for a card whose boot sector could not
 * be READ, not only for one that genuinely has no filesystem - and read
 * corruption is a demonstrated failure mode on this hardware (SDMMC data CRC
 * errors). With auto-format enabled, one corrupted read of the boot sector is
 * enough to erase the card. Set to 1 only for a bring-up rig with scratch
 * media.
 */
#ifndef WAVEX_DAISY_SD_AUTO_FORMAT
#define WAVEX_DAISY_SD_AUTO_FORMAT 0
#endif

// SD Card debug logging (Daisy only)
// TEMPORARY (2026-08-28): both debug streams enabled to exercise the
// non-blocking log ring under playback load. Revert both to 0 once that has
// been confirmed on hardware - verbose logging is not a shipping default.
#ifndef WAVEX_DAISY_SD_DEBUG
#define WAVEX_DAISY_SD_DEBUG 1
#endif

// Audio streaming telemetry (Daisy only): periodic STREAM/STREAM2 lines
// reporting pre-buffer fill, WAV format, output peaks, and why a streaming
// pass discarded without consuming. Built for the 2026-08 audition stalls
// (non-48 kHz files deadlocking in the resample path) and kept behind this
// flag because that class of bug is invisible without it. Meant to stay off
// outside debugging (see the TEMPORARY note above): each line is a blocking
// USB CDC write on the loop that refills the audio ring, so leaving it on
// during playback starves the refill.
#ifndef WAVEX_DAISY_STREAM_DEBUG
#define WAVEX_DAISY_STREAM_DEBUG 1
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
 * @brief Selects the SDMMC bus clock, as a SdmmcHandler::Speed enumerator.
 *
 * Bus clock per setting. These are derived, not estimated:
 *   SDMMC_CK = sdmmc_ker_ck / (2 x ClockDiv)          [RM0433, SDMMC_CLKCR]
 *   sdmmc_ker_ck = PLL2R = 200 MHz
 *     (RCC_SDMMCCLKSOURCE_PLL2 with HSE 16 MHz, PLL2 M=1, N=12,
 *      FRACN=4096 -> VCO 16 x 12.5 = 200 MHz, R=1; libDaisy system.cpp)
 *   ClockDiv per enumerator from libDaisy per/sdmmc.cpp:21-25.
 *
 * - 0: SLOW         ClockDiv 250 ->   400 kHz  (also the mandatory
 *                                              identification-phase rate)
 * - 1: MEDIUM_SLOW  ClockDiv 8   ->  12.5 MHz
 * - 2: STANDARD     ClockDiv 4   ->    25 MHz  (SD Default Speed)
 * - 3: FAST         ClockDiv 2   ->    50 MHz  (SD High Speed)
 * - 4: VERY_FAST    ClockDiv 1   ->   100 MHz  (SDR50, overclocked)
 *
 * At 4-bit width the theoretical ceiling is SDMMC_CK x 4 bits, so STANDARD
 * is 12.5 MB/s. Measured throughput is far below that (~2.8 MB/s for an 8 KiB
 * f_read) because most of a read is command and card-state polling overhead,
 * not transfer - see the busy-wait loops in libDaisy's sd_diskio.c SD_read().
 * Raising this setting only shrinks the transfer portion, so expect roughly a
 * 10% improvement from STANDARD to FAST, not 2x.
 *
 * This is the STARTING point, not a fixed setting. SdSdio::InitAndMount()
 * negotiates down from here until the card mounts and reads, and a data CRC
 * failure under load steps it down again at runtime
 * (SdSdio::DowngradeSpeed()).
 *
 * Set to 3 (FAST): reach for 50 MHz and let negotiation settle lower if this
 * board cannot hold it. Watch for "SD: negotiated DOWN" (fell back at boot)
 * or "SD: downgrading" (fell back later, under load) - either means the card
 * or wiring is not holding the configured rate, and the line names the rate
 * it settled on.
 */
#ifndef WAVEX_DAISY_SD_CARD_SPEED
#define WAVEX_DAISY_SD_CARD_SPEED 3
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

// TCA8418 I2C keypad controller (up to 8 rows x 10 columns, hardware
// debounce, event FIFO, INT). Panel keys, encoder push switches and - with
// the Phase 2 pad grid - the 16 pads all go through it (panel-controls.md).
#ifndef WAVEX_ESP_BUTTON_MATRIX_ENABLED
#define WAVEX_ESP_BUTTON_MATRIX_ENABLED 1
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

// DIN MIDI Input (ESP32 UART2 - pins/baud in pin_config.h)
//
// OFF since 2026-09-04: WAVEX_ESP_MIDI_RX was GPIO24, which on the ESP32-P4
// is also USB D- of the chip's built-in USB-Serial/JTAG port - the fast flash
// path. With that cable plugged in UART2 sampled USB traffic and the parser
// forwarded it to the Daisy as ~700 note-on/off per second (note numbers
// 0/4/8/32/64). The pins moved on 2026-09-05 (pin_config.h); this stays 0
// until the DIN receiver is physically on the new RX pin - an open UART RX
// input reads noise, and the storm detector in midi_task.cpp would be all
// that stood between that and the Daisy. USB MIDI
// (WAVEX_ESP_USB_MIDI_ENABLED) is a different controller and unaffected.
#ifndef WAVEX_ESP_DIN_MIDI_ENABLED
#define WAVEX_ESP_DIN_MIDI_ENABLED 0
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

// Audio sample rate configuration (48 kHz: preserves the 1-block = 1-ms
// control-tick invariant with 48-sample blocks - architecture.md §5.1)
#ifndef WAVEX_AUDIO_SAMPLE_RATE
#define WAVEX_AUDIO_SAMPLE_RATE 48000
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
#define WAVEX_DSI_LANE_BITRATE_MBPS 1500
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

// WaveX logical PCNT unit index. The pulse_cnt driver allocates hardware
// units as opaque handles, so this is an index into WaveX's own table, not a
// hardware unit number. Channel selection is gone with the legacy driver:
// both quadrature channels are allocated from the unit handle.
#ifndef WAVEX_ENCODER_PCNT_UNIT
#define WAVEX_ENCODER_PCNT_UNIT 0
#endif

// Encoder filter configuration
#ifndef WAVEX_ENCODER_FILTER_ENABLED
#define WAVEX_ENCODER_FILTER_ENABLED 1
#endif

// Glitch filter width. The legacy driver counted APB clock cycles (800 @
// 80 MHz = 10 us); pulse_cnt takes nanoseconds directly, so the ns value is
// now the source of truth and the cycle count is retained only for reference.
#ifndef WAVEX_ENCODER_FILTER_VALUE
#define WAVEX_ENCODER_FILTER_VALUE 800
#endif

#ifndef WAVEX_ENCODER_FILTER_NS
#define WAVEX_ENCODER_FILTER_NS 10000
#endif
#endif

// PCNT1 Configuration
#if WAVEX_ESP_PCNT1_ENABLED
// WaveX logical PCNT unit index for PCNT1 (see WAVEX_ENCODER_PCNT_UNIT).
#ifndef WAVEX_PCNT1_UNIT
#define WAVEX_PCNT1_UNIT 1
#endif

// PCNT1 filter configuration
#ifndef WAVEX_PCNT1_FILTER_ENABLED
#define WAVEX_PCNT1_FILTER_ENABLED 1
#endif

#ifndef WAVEX_PCNT1_FILTER_VALUE
#define WAVEX_PCNT1_FILTER_VALUE 800
#endif

#ifndef WAVEX_PCNT1_FILTER_NS
#define WAVEX_PCNT1_FILTER_NS 10000
#endif

// Quadrature counts per mechanical detent. The PEC11R is decoded 4x (both
// edges of both signals), so one detent click moves the counter by 4.
#ifndef WAVEX_PCNT1_COUNTS_PER_DETENT
#define WAVEX_PCNT1_COUNTS_PER_DETENT 4
#endif

// PCNT1 interrupt thresholds
#ifndef WAVEX_PCNT1_THRESH_POS
#define WAVEX_PCNT1_THRESH_POS 4
#endif

#ifndef WAVEX_PCNT1_THRESH_NEG
#define WAVEX_PCNT1_THRESH_NEG -4
#endif
#endif
// Panel LEDs (TLC5947 chain), endless pots (MCP3008) and the button matrix
// sizing. PLANNED, not as-built: no driver reads these yet. The design that
// consumes them is docs/features/panel-controls.md; 48 channels = two chained
// TLC5947s, four endless pots = the eight channels of one MCP3008.
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

// DIN MIDI Configuration
#if WAVEX_ESP_DIN_MIDI_ENABLED
// Above the UI task, below the inter-MCU uart_link task (6): a note must
// preempt rendering to make item 8's < 5 ms in-to-sound budget, but must
// never starve the link that carries it onward.
#ifndef WAVEX_DIN_MIDI_TASK_PRIORITY
#define WAVEX_DIN_MIDI_TASK_PRIORITY 5
#endif

#ifndef WAVEX_DIN_MIDI_TASK_STACK_SIZE
#define WAVEX_DIN_MIDI_TASK_STACK_SIZE 4096
#endif

// UART driver RX ring. MIDI is 3125 bytes/s max; 256 bytes is ~80 ms of
// worst-case backlog, far more than the task ever lets accumulate.
#ifndef WAVEX_DIN_MIDI_RX_BUF_SIZE
#define WAVEX_DIN_MIDI_RX_BUF_SIZE 256
#endif
#endif

// TCA8418 Button Matrix Configuration
#if WAVEX_ESP_BUTTON_MATRIX_ENABLED
// I2C configuration
// I2C address of the keypad controller. The bus itself belongs to the BSP
// (shared with the touch controller), which is why the port/clock macros below
// are unused - they describe a bus this firmware does not open itself.
#ifndef WAVEX_TCA8418_I2C_ADDR
#define WAVEX_TCA8418_I2C_ADDR 0x34
#endif

#ifndef WAVEX_TCA8418_I2C_PORT
#define WAVEX_TCA8418_I2C_PORT I2C_NUM_0
#endif

#ifndef WAVEX_TCA8418_I2C_CLOCK_SPEED
#define WAVEX_TCA8418_I2C_CLOCK_SPEED (400 * 1000)  // 400 kHz
#endif

// Button matrix dimensions
// Matrix geometry passed to the controller. These describe what the firmware
// currently configures, not a verified reading of the schematic: the columns
// value in particular is unconfirmed (see roadmap § Outstanding hardware
// verification). Change them here, not at the call site.
#ifndef WAVEX_TCA8418_ROWS
#define WAVEX_TCA8418_ROWS 8
#endif

#ifndef WAVEX_TCA8418_COLUMNS
#define WAVEX_TCA8418_COLUMNS 10
#endif

// Task configuration
#ifndef WAVEX_TCA8418_TASK_PRIORITY
#define WAVEX_TCA8418_TASK_PRIORITY 5
#endif

#ifndef WAVEX_TCA8418_TASK_STACK_SIZE
#define WAVEX_TCA8418_TASK_STACK_SIZE 4096
#endif
#endif

// ============================================================================
// DEPENDENCY CHECKS
// ============================================================================

// Ensure inter-MCU link is enabled if any component that depends on it is enabled.
//
// Five of the names this used to test did not exist - WAVEX_ENCODER_PCNT_ENABLED,
// WAVEX_PCNT1_ENABLED, WAVEX_4067_MUX_ENABLED, WAVEX_TCA8418_BUTTON_MATRIX_ENABLED
// and WAVEX_USB_MIDI_ENABLED - and an undefined identifier in #if expands to 0,
// so the guard silently covered only the audio engine and the LCD. The real
// names all carry the WAVEX_ESP_ prefix. (The CD74HC4067 mux flag was removed
// 2026-09-05 along with the part: its plan predated the MCP3008 and its
// address pins were not on this board's header.)
#if (WAVEX_AUDIO_ENGINE_ENABLED || WAVEX_ESP_ENCODER_PCNT_ENABLED || WAVEX_ESP_PCNT1_ENABLED || \
     WAVEX_ESP_BUTTON_MATRIX_ENABLED || WAVEX_LCD_DISPLAY_ENABLED ||                            \
     WAVEX_ESP_USB_MIDI_ENABLED) &&                                                             \
    !WAVEX_INTER_MCU_LINK_ENABLED
#error "Inter-MCU link must be enabled when using components that depend on it"
#endif
