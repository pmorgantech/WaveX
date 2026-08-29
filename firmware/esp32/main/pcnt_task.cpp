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

static const char *TAG = "PCNT_TASK";

// PCNT unit configurations
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

// Encoder readings storage
static encoder_reading_t s_encoder_readings[WAVEX_PCNT_UNIT_COUNT] = {};

// Driver handles, indexed by WaveX logical unit. NULL means "not initialized".
static pcnt_unit_handle_t s_pcnt_units[WAVEX_PCNT_UNIT_COUNT] = {};

// Task handle
static TaskHandle_t s_pcnt_task_handle = NULL;

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
    ESP_LOGI(TAG, "PCNT monitoring task started (polling-based for reliable operation)");

    while (1) {
        // Poll encoder counters for changes
        for (int i = 0; i < PCNT_CONFIG_COUNT; i++) {
            const wavex_pcnt_config_t *config = &s_pcnt_configs[i];
            if (!config->enabled || s_pcnt_units[config->unit] == NULL) {
                continue;
            }

            encoder_reading_t *reading = &s_encoder_readings[config->unit];

            // Read current hardware counter value
            int hw_count = 0;
            pcnt_unit_get_count(s_pcnt_units[config->unit], &hw_count);

            // Calculate delta since last poll
            int32_t delta = (int32_t)hw_count - reading->count;
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

                // For now, process all deltas to restore functionality
                // TODO: Add noise filtering back once we understand the delta patterns
                reading->prev_count = reading->count;
                reading->count = (int32_t)hw_count;
                reading->delta += delta;

                // Clear hardware counter to prevent overflow
                pcnt_unit_clear_count(s_pcnt_units[config->unit]);
                reading->count = 0;
                reading->prev_count = 0;
            }
        }

        // Brief delay for polling frequency (faster polling for better responsiveness)
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t pcnt_task_init(void) {
    ESP_LOGI(TAG, "Initializing PCNT task...");

    // Initialize all enabled PCNT units
    for (int i = 0; i < PCNT_CONFIG_COUNT; i++) {
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

    // Create PCNT reading task
    BaseType_t ret = xTaskCreate(pcnt_task,           // Task function
                                 "pcnt_task",         // Task name
                                 4096,                // Stack size
                                 NULL,                // Parameters
                                 5,                   // Priority (higher than UI task)
                                 &s_pcnt_task_handle  // Task handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PCNT task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PCNT reading task started successfully");
    return ESP_OK;
}

esp_err_t pcnt_task_stop(void) {
    if (s_pcnt_task_handle != NULL) {
        vTaskDelete(s_pcnt_task_handle);
        s_pcnt_task_handle = NULL;
        ESP_LOGI(TAG, "PCNT task stopped");
    }
    return ESP_OK;
}

esp_err_t pcnt_get_reading(uint8_t unit, encoder_reading_t *reading) {
    if (unit >= WAVEX_PCNT_UNIT_COUNT || reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check if unit is enabled
    bool unit_enabled = false;
    for (int i = 0; i < PCNT_CONFIG_COUNT; i++) {
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
        s_encoder_readings[unit].count = 0;
        s_encoder_readings[unit].prev_count = 0;
        s_encoder_readings[unit].delta = 0;
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
    // Fetch and clear atomically with interrupts disabled to avoid ISR race.
    uint32_t prev_level = portSET_INTERRUPT_MASK_FROM_ISR();
    int32_t delta = s_encoder_readings[unit].delta;
    s_encoder_readings[unit].delta = 0;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(prev_level);
    return delta;
}
