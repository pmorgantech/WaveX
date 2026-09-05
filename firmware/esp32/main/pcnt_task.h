/**
 * @file pcnt_task.h
 * @brief PCNT (Pulse Counter) Task for ESP32 Encoder Handling
 *
 * This module provides polled PCNT-based encoder reading for the main encoder
 * and a second quadrature input, built on the `driver/pulse_cnt.h` API.
 *
 * Features:
 * - Hardware quadrature decoding (4x, both channels, both edges)
 * - Hardware glitch filter for contact-bounce immunity
 * - Accumulated deltas consumed by the UI task
 *
 * The legacy `driver/pcnt.h` API this module used previously was removed in
 * ESP-IDF v6.0. The replacement allocates units and channels as opaque
 * handles rather than naming fixed hardware indices, so units are identified
 * here by a WaveX-local index (see WAVEX_PCNT_UNIT_COUNT) that the
 * WAVEX_*_PCNT_UNIT macros in hardware_config.h map onto.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of logical PCNT units WaveX manages (main encoder + aux). */
#define WAVEX_PCNT_UNIT_COUNT 2

/**
 * @brief PCNT unit configuration structure
 *
 * There is no channel index: the pulse_cnt driver allocates both quadrature
 * channels internally from the unit handle.
 */
typedef struct {
    uint8_t unit;            // WaveX logical unit index (< WAVEX_PCNT_UNIT_COUNT)
    int gpio_a;              // GPIO pin for signal A
    int gpio_b;              // GPIO pin for signal B
    uint32_t max_glitch_ns;  // Glitch filter width in nanoseconds
    int8_t direction;        // +1 or -1: applied to every count so clockwise is
                             // positive for all consumers (hardware_config.h)
    bool enabled;            // Whether this unit is enabled
} wavex_pcnt_config_t;

typedef struct {
    int32_t last_hw;     // Hardware count at the previous poll
    int32_t prev_count;  // Unused; retained so the struct layout is unchanged
    // Accumulated movement not yet consumed. Written by the PCNT task and
    // taken by the UI task, on either core, so every access goes through
    // __atomic_* builtins rather than a plain read-modify-write. Kept as a
    // plain int32_t here because this header is `extern "C"`.
    int32_t delta;
} encoder_reading_t;

esp_err_t pcnt_task_init(void);
esp_err_t pcnt_task_start(void);
esp_err_t pcnt_task_stop(void);

esp_err_t pcnt_get_reading(uint8_t unit, encoder_reading_t *reading);

esp_err_t pcnt_reset_counter(uint8_t unit);

/** Raw hardware counter value for `unit`, bypassing delta tracking. Debugging only. */
esp_err_t pcnt_get_raw_count(uint8_t unit, int *count);

/**
 * @brief Atomically fetch and clear the most recent encoder delta for a unit
 *
 * This returns the accumulated delta since the last call and resets it to 0.
 * If the unit is disabled or invalid, returns 0.
 */
int32_t pcnt_consume_delta(uint8_t unit);

#ifdef __cplusplus
}
#endif
