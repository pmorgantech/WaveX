/**
 * @file pcnt_task.h
 * @brief PCNT (Pulse Counter) Task for ESP32 Encoder Handling
 *
 * This module provides interrupt-driven PCNT-based encoder reading functionality for both
 * the main encoder (PCNT_UNIT_0) and additional PCNT unit (PCNT_UNIT_1).
 *
 * Features:
 * - Interrupt-driven encoder counting for low latency
 * - Threshold interrupts every 4 counts (typical encoder detent)
 * - Zero-crossing detection for full rotation tracking
 * - Glitch filtering for noise immunity
 * - Quadrature decoding for direction sensing
 * - Continuous change monitoring with immediate logging
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/pcnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PCNT unit configuration structure
 */
typedef struct {
    pcnt_unit_t unit;           // PCNT unit number
    pcnt_channel_t channel_a;   // Channel for signal A
    pcnt_channel_t channel_b;   // Channel for signal B
    int gpio_a;                 // GPIO pin for signal A
    int gpio_b;                 // GPIO pin for signal B
    int filter_value;           // Glitch filter value
    bool enabled;               // Whether this unit is enabled
} wavex_pcnt_config_t;

/**
 * @brief Encoder reading structure
 */
typedef struct {
    int32_t count;      // Current counter value
    int32_t prev_count; // Previous counter value for delta calculation
    int32_t delta;      // Change since last read
} encoder_reading_t;

/**
 * @brief Initialize PCNT peripherals
 *
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t pcnt_task_init(void);

/**
 * @brief Start PCNT reading task
 *
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t pcnt_task_start(void);

/**
 * @brief Stop PCNT reading task
 *
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t pcnt_task_stop(void);

/**
 * @brief Get current encoder reading for specified unit
 *
 * @param unit PCNT unit to read from
 * @param reading Pointer to store the reading
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t pcnt_get_reading(pcnt_unit_t unit, encoder_reading_t *reading);

/**
 * @brief Reset encoder counter for specified unit
 *
 * @param unit PCNT unit to reset
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t pcnt_reset_counter(pcnt_unit_t unit);

/**
 * @brief Get raw PCNT counter value (for debugging)
 *
 * @param unit PCNT unit to read from
 * @param count Pointer to store the count
 * @return ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t pcnt_get_raw_count(pcnt_unit_t unit, int16_t *count);

/**
 * @brief Atomically fetch and clear the most recent encoder delta for a unit
 *
 * This returns the accumulated delta since the last call and resets it to 0.
 * If the unit is disabled or invalid, returns 0.
 */
int32_t pcnt_consume_delta(pcnt_unit_t unit);

#ifdef __cplusplus
}
#endif
