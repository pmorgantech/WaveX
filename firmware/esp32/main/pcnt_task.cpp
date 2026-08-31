/**
 * @file pcnt_task.cpp
 * @brief PCNT (Pulse Counter) Task Implementation for ESP32 Encoder Handling
 *
 * Polled quadrature decoding for the main encoder and a second PCNT input,
 * built on `driver/pulse_cnt.h`. Units and channels are opaque handles owned
 * by this module; callers address them through the WaveX logical unit index.
 */

#include "pcnt_task.h"

#include <inttypes.h>
#include <stdint.h>

#include "../../shared/config/hardware_config.h"
#include "../../shared/config/pin_config.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"

#include <atomic>

static const char *TAG = "PCNT_TASK";

static wavex_pcnt_config_t s_pcnt_configs[] = {
    // Main encoder
    {.unit = WAVEX_ENCODER_PCNT_UNIT,
     .gpio_a = WAVEX_ESP_ENCODER_A,
     .gpio_b = WAVEX_ESP_ENCODER_B,
     .max_glitch_ns = WAVEX_ENCODER_FILTER_NS,
     .enabled = WAVEX_ESP_ENCODER_PCNT_ENABLED},
    // Additional PCNT unit
    {.unit = WAVEX_PCNT1_UNIT,
     .gpio_a = WAVEX_ESP_PCNT1_A,
     .gpio_b = WAVEX_ESP_PCNT1_B,
     .max_glitch_ns = WAVEX_PCNT1_FILTER_NS,
     .enabled = WAVEX_ESP_PCNT1_ENABLED}};

#define PCNT_CONFIG_COUNT (sizeof(s_pcnt_configs) / sizeof(wavex_pcnt_config_t))

static encoder_reading_t s_encoder_readings[WAVEX_PCNT_UNIT_COUNT] = {};

// Driver handles, indexed by WaveX logical unit. NULL means "not initialized".
static pcnt_unit_handle_t s_pcnt_units[WAVEX_PCNT_UNIT_COUNT] = {};

static std::atomic<TaskHandle_t> s_pcnt_task_handle{NULL};
// Shutdown handshake; see midi_task.cpp. This task touches PCNT driver
// internals, so it has to leave its loop on its own rather than be deleted.
static std::atomic<bool> s_pcnt_running{false};

/**
 * @brief Initialize a single PCNT unit
 *
 * Quadrature decoding uses two channels that mirror each other: each counts
 * both edges of one signal while taking its direction from the level of the
 * other. This reproduces the legacy driver's pos/neg + lctrl/hctrl matrix
 * (INC/DEC on A, DEC/INC on B, control-high reverses, control-low keeps).
 */
static esp_err_t pcnt_init_unit(const wavex_pcnt_config_t *config) {
    ESP_LOGI(TAG,
             "Initializing PCNT unit %u (GPIO A:%d, B:%d) - quadrature mode for PEC11R",
             (unsigned)config->unit,
             config->gpio_a,
             config->gpio_b);

    pcnt_unit_config_t unit_config = {};
    unit_config.high_limit = INT16_MAX;
    unit_config.low_limit = INT16_MIN;

    pcnt_unit_handle_t unit = NULL;
    esp_err_t ret = pcnt_new_unit(&unit_config, &unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to allocate PCNT unit %u: %s",
                 (unsigned)config->unit,
                 esp_err_to_name(ret));
        return ret;
    }

    // Glitch filter: the legacy driver counted APB clock cycles, the current
    // one takes nanoseconds directly (see WAVEX_ENCODER_FILTER_NS).
    pcnt_glitch_filter_config_t filter_config = {};
    filter_config.max_glitch_ns = config->max_glitch_ns;
    ret = pcnt_unit_set_glitch_filter(unit, &filter_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to set glitch filter for PCNT unit %u: %s",
                 (unsigned)config->unit,
                 esp_err_to_name(ret));
        pcnt_del_unit(unit);
        return ret;
    }

    // Channel A: edges on signal A, direction from the level of signal B.
    pcnt_chan_config_t chan_a_config = {};
    chan_a_config.edge_gpio_num = config->gpio_a;
    chan_a_config.level_gpio_num = config->gpio_b;
    pcnt_channel_handle_t chan_a = NULL;
    ret = pcnt_new_channel(unit, &chan_a_config, &chan_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to create PCNT unit %u channel A: %s",
                 (unsigned)config->unit,
                 esp_err_to_name(ret));
        pcnt_del_unit(unit);
        return ret;
    }
    ret = pcnt_channel_set_edge_action(
        chan_a, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    if (ret == ESP_OK) {
        ret = pcnt_channel_set_level_action(
            chan_a, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to configure PCNT unit %u channel A: %s",
                 (unsigned)config->unit,
                 esp_err_to_name(ret));
        pcnt_del_channel(chan_a);
        pcnt_del_unit(unit);
        return ret;
    }

    // Channel B: mirror of A - edges on signal B, direction from signal A.
    pcnt_chan_config_t chan_b_config = {};
    chan_b_config.edge_gpio_num = config->gpio_b;
    chan_b_config.level_gpio_num = config->gpio_a;
    pcnt_channel_handle_t chan_b = NULL;
    ret = pcnt_new_channel(unit, &chan_b_config, &chan_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to create PCNT unit %u channel B: %s",
                 (unsigned)config->unit,
                 esp_err_to_name(ret));
        pcnt_del_channel(chan_a);
        pcnt_del_unit(unit);
        return ret;
    }
    ret = pcnt_channel_set_edge_action(
        chan_b, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    if (ret == ESP_OK) {
        ret = pcnt_channel_set_level_action(
            chan_b, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to configure PCNT unit %u channel B: %s",
                 (unsigned)config->unit,
                 esp_err_to_name(ret));
        pcnt_del_channel(chan_b);
        pcnt_del_channel(chan_a);
        pcnt_del_unit(unit);
        return ret;
    }

    // Enable, zero, and start. The legacy sequence was pause/clear/resume;
    // the current driver additionally requires an explicit enable before the
    // unit will accept a start.
    ret = pcnt_unit_enable(unit);
    if (ret == ESP_OK) {
        ret = pcnt_unit_clear_count(unit);
    }
    if (ret == ESP_OK) {
        ret = pcnt_unit_start(unit);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG, "Failed to start PCNT unit %u: %s", (unsigned)config->unit, esp_err_to_name(ret));
        pcnt_unit_disable(unit);
        pcnt_del_channel(chan_b);
        pcnt_del_channel(chan_a);
        pcnt_del_unit(unit);
        return ret;
    }

    // Channel handles are not retained: they live as long as the unit, and
    // nothing reconfigures them after init.
    s_pcnt_units[config->unit] = unit;

    ESP_LOGI(TAG, "PCNT unit %u initialized successfully", (unsigned)config->unit);
    return ESP_OK;
}

/**
 * @brief PCNT monitoring task (polling-based for reliable encoder reading)
 */
static void pcnt_task(void *pvParameters) {
    (void)pvParameters;
    ESP_LOGI(TAG, "PCNT monitoring task started (polling-based for reliable operation)");

    while (s_pcnt_running) {
        for (size_t i = 0; i < PCNT_CONFIG_COUNT; i++) {
            const wavex_pcnt_config_t *config = &s_pcnt_configs[i];
            if (!config->enabled || s_pcnt_units[config->unit] == NULL) {
                continue;
            }

            encoder_reading_t *reading = &s_encoder_readings[config->unit];

            int hw_count = 0;
            esp_err_t get_err = pcnt_unit_get_count(s_pcnt_units[config->unit], &hw_count);
            if (get_err != ESP_OK) {
                // Leave last_hw alone: the next successful read then reports
                // the movement across both polls instead of losing it.
                ESP_LOGW(TAG,
                         "PCNT unit %u get_count failed: %s",
                         (unsigned)config->unit,
                         esp_err_to_name(get_err));
                continue;
            }

            int32_t delta = (int32_t)hw_count - reading->last_hw;
            reading->last_hw = (int32_t)hw_count;
            if (delta != 0) {
                const char *unit_name = (config->unit == WAVEX_ENCODER_PCNT_UNIT)
                                            ? "Main Encoder"
                                            : "PCNT1 Encoder (PEC11R quadrature)";
                // DEBUG, not INFO: this fires on every 2ms poll while a knob
                // turns, which at console baud rate would stall this task.
                ESP_LOGD(TAG,
                         "%s - Count: %" PRId32 ", Delta: %" PRId32,
                         unit_name,
                         (int32_t)hw_count,
                         delta);

                // Atomic add: the UI task takes this with an exchange from the
                // other core, and this task is unpinned. A plain `+=` here let
                // a consumer's zeroing land between the read and the write, so
                // detents were silently dropped under load. Relaxed ordering is
                // enough - the delta is a self-contained count, not a flag
                // publishing some other buffer.
                __atomic_fetch_add(&reading->delta, delta, __ATOMIC_RELAXED);
            }

            // Re-centre well before the driver's ±INT16 limit, where it would
            // reset the count to zero on its own and make the next delta a
            // large bogus jump.
            //
            // The counter is NOT cleared on every poll any more. Doing that
            // discarded any edge landing between get_count() and clear_count(),
            // which is every poll during movement; now the window is hit once
            // per ~8000 counts (~85 revolutions), where losing a fraction of a
            // detent is imperceptible. Closing it completely needs the driver's
            // watch-point callbacks, which is an ISR and wants bench time.
            constexpr int32_t kRecentreThreshold = 8000;
            if (hw_count > kRecentreThreshold || hw_count < -kRecentreThreshold) {
                esp_err_t clear_err = pcnt_unit_clear_count(s_pcnt_units[config->unit]);
                if (clear_err == ESP_OK) {
                    reading->last_hw = 0;
                } else {
                    // last_hw still matches the hardware, so the baseline stays
                    // true and the next poll just tries again. The old code
                    // zeroed it regardless, which re-applied the whole count as
                    // fresh delta on every subsequent poll.
                    ESP_LOGW(TAG,
                             "PCNT unit %u clear_count failed: %s",
                             (unsigned)config->unit,
                             esp_err_to_name(clear_err));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    s_pcnt_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t pcnt_task_init(void) {
    ESP_LOGI(TAG, "Initializing PCNT task...");

    for (size_t i = 0; i < PCNT_CONFIG_COUNT; i++) {
        const wavex_pcnt_config_t *config = &s_pcnt_configs[i];
        if (config->enabled) {
            esp_err_t ret = pcnt_init_unit(config);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize PCNT unit %u", (unsigned)config->unit);
                return ret;
            }
        }
    }

    ESP_LOGI(TAG, "PCNT task initialization completed");
    return ESP_OK;
}

esp_err_t pcnt_task_start(void) {
    ESP_LOGI(TAG, "Starting PCNT reading task...");

    s_pcnt_running = true;
    TaskHandle_t handle = NULL;
    BaseType_t ret = xTaskCreate(pcnt_task,    // Task function
                                 "pcnt_task",  // Task name
                                 4096,         // Stack size
                                 NULL,         // Parameters
                                 5,            // Priority (higher than UI task)
                                 &handle       // Task handle
    );
    s_pcnt_task_handle = handle;

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PCNT task");
        s_pcnt_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PCNT reading task started successfully");
    return ESP_OK;
}

esp_err_t pcnt_task_stop(void) {
    s_pcnt_running = false;
    for (int waited_ms = 0; s_pcnt_task_handle != NULL && waited_ms < 200; waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_pcnt_task_handle != NULL) {
        ESP_LOGE(TAG, "PCNT task did not exit");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "PCNT task stopped");
    return ESP_OK;
}

esp_err_t pcnt_get_reading(uint8_t unit, encoder_reading_t *reading) {
    if (unit >= WAVEX_PCNT_UNIT_COUNT || reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool unit_enabled = false;
    for (size_t i = 0; i < PCNT_CONFIG_COUNT; i++) {
        if (s_pcnt_configs[i].unit == unit && s_pcnt_configs[i].enabled) {
            unit_enabled = true;
            break;
        }
    }

    if (!unit_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    *reading = s_encoder_readings[unit];
    return ESP_OK;
}

esp_err_t pcnt_reset_counter(uint8_t unit) {
    if (unit >= WAVEX_PCNT_UNIT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pcnt_units[unit] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = pcnt_unit_clear_count(s_pcnt_units[unit]);
    if (ret == ESP_OK) {
        s_encoder_readings[unit].last_hw = 0;
        s_encoder_readings[unit].prev_count = 0;
        __atomic_store_n(&s_encoder_readings[unit].delta, 0, __ATOMIC_RELAXED);
    }
    return ret;
}

esp_err_t pcnt_get_raw_count(uint8_t unit, int *count) {
    if (unit >= WAVEX_PCNT_UNIT_COUNT || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pcnt_units[unit] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return pcnt_unit_get_count(s_pcnt_units[unit], count);
}

int32_t pcnt_consume_delta(uint8_t unit) {
    if (unit >= WAVEX_PCNT_UNIT_COUNT) {
        return 0;
    }
    // Fetch-and-clear in one atomic step.
    //
    // This previously bracketed a plain read and write with
    // portSET_INTERRUPT_MASK_FROM_ISR(), described as avoiding "an ISR race".
    // There is no ISR - the producer is pcnt_task - and masking interrupts
    // only affects the calling core, so it did nothing about a producer
    // running on the other one. That is the anti-pattern the ESP32-P4 guide
    // opens with (§1).
    return __atomic_exchange_n(&s_encoder_readings[unit].delta, 0, __ATOMIC_RELAXED);
}
