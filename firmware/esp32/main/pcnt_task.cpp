/**
 * @file pcnt_task.cpp
 * @brief PCNT (Pulse Counter) Task Implementation for ESP32 Encoder Handling
 *
 * This module provides PCNT-based encoder reading functionality for both
 * the main encoder (PCNT_UNIT_0) and additional PCNT unit (PCNT_UNIT_1).
 */

#include "pcnt_task.h"

#include "../../shared/config/hardware_config.h"
#include "../../shared/config/pin_config.h"
#include "driver/pcnt.h"
#include "esp_log.h"

// For interrupt handling
#include "esp_intr_alloc.h"
#include "soc/pcnt_reg.h"

static const char *TAG = "PCNT_TASK";

// PCNT unit configurations
static wavex_pcnt_config_t s_pcnt_configs[] = {
    // Main encoder (PCNT_UNIT_0)
    {.unit = WAVEX_ENCODER_PCNT_UNIT,
     .channel_a = WAVEX_ENCODER_PCNT_CH_A,
     .channel_b = WAVEX_ENCODER_PCNT_CH_B,
     .gpio_a = WAVEX_ESP_ENCODER_A,
     .gpio_b = WAVEX_ESP_ENCODER_B,
     .filter_value = WAVEX_ENCODER_FILTER_VALUE,
     .enabled = WAVEX_ESP_ENCODER_PCNT_ENABLED},
    // Additional PCNT unit (PCNT_UNIT_1)
    {.unit = WAVEX_PCNT1_UNIT,
     .channel_a = WAVEX_PCNT1_CH_A,
     .channel_b = WAVEX_PCNT1_CH_B,
     .gpio_a = WAVEX_ESP_PCNT1_A,
     .gpio_b = WAVEX_ESP_PCNT1_B,
     .filter_value = WAVEX_PCNT1_FILTER_VALUE,
     .enabled = WAVEX_ESP_PCNT1_ENABLED}};

#define PCNT_CONFIG_COUNT (sizeof(s_pcnt_configs) / sizeof(wavex_pcnt_config_t))

// Encoder readings storage
static encoder_reading_t s_encoder_readings[PCNT_UNIT_MAX] = {0};

// Task handle
static TaskHandle_t s_pcnt_task_handle = NULL;

/**
 * @brief Initialize a single PCNT unit
 */
static esp_err_t pcnt_init_unit(const wavex_pcnt_config_t *config) {
    ESP_LOGI(TAG,
             "Initializing PCNT unit %d (GPIO A:%d, B:%d) - quadrature mode for PEC11R",
             config->unit,
             config->gpio_a,
             config->gpio_b);

    // Configure PCNT unit for PEC11R quadrature encoder
    // Try alternative quadrature configuration for PEC11R
    pcnt_config_t esp_pcnt_config = {
        .pulse_gpio_num = config->gpio_a,
        .ctrl_gpio_num = config->gpio_b,
        .lctrl_mode = PCNT_MODE_KEEP,     // Try KEEP instead of REVERSE
        .hctrl_mode = PCNT_MODE_REVERSE,  // Try REVERSE instead of KEEP
        .pos_mode = PCNT_COUNT_INC,       // Count up on positive edge
        .neg_mode = PCNT_COUNT_DEC,       // Count down on negative edge
        .counter_h_lim = INT16_MAX,
        .counter_l_lim = INT16_MIN,
        .unit = config->unit,
        .channel = config->channel_a};

    esp_err_t ret = pcnt_unit_config(&esp_pcnt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to configure PCNT unit %d channel A: %s",
                 config->unit,
                 esp_err_to_name(ret));
        return ret;
    }

    // Configure channel B for quadrature decoding (alternative config)
    esp_pcnt_config.pulse_gpio_num = config->gpio_b;
    esp_pcnt_config.ctrl_gpio_num = config->gpio_a;
    esp_pcnt_config.channel = config->channel_b;
    esp_pcnt_config.pos_mode = PCNT_COUNT_DEC;  // Standard for channel B
    esp_pcnt_config.neg_mode = PCNT_COUNT_INC;  // Standard for channel B

    ret = pcnt_unit_config(&esp_pcnt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to configure PCNT unit %d channel B: %s",
                 config->unit,
                 esp_err_to_name(ret));
        return ret;
    }

    // Configure glitch filter
    ret = pcnt_set_filter_value(config->unit, config->filter_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to set filter value for PCNT unit %d: %s",
                 config->unit,
                 esp_err_to_name(ret));
        return ret;
    }

    ret = pcnt_filter_enable(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to enable filter for PCNT unit %d: %s",
                 config->unit,
                 esp_err_to_name(ret));
        return ret;
    }

    // Initialize counter
    ret = pcnt_counter_pause(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to pause PCNT unit %d: %s", config->unit, esp_err_to_name(ret));
        return ret;
    }

    ret = pcnt_counter_clear(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear PCNT unit %d: %s", config->unit, esp_err_to_name(ret));
        return ret;
    }

    // Start counter
    ret = pcnt_counter_resume(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resume PCNT unit %d: %s", config->unit, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PCNT unit %d initialized successfully", config->unit);
    return ESP_OK;
}

// ISR handler removed - using polling approach instead

/**
 * @brief PCNT monitoring task (polling-based for reliable encoder reading)
 */
static void pcnt_task(void *pvParameters) {
    ESP_LOGI(TAG, "PCNT monitoring task started (polling-based for reliable operation)");

    // No ISR service needed for polling approach
    // We'll periodically check counter values instead of using interrupts

    while (1) {
        // Poll encoder counters for changes
        for (int i = 0; i < PCNT_CONFIG_COUNT; i++) {
            const wavex_pcnt_config_t *config = &s_pcnt_configs[i];
            if (!config->enabled) {
                continue;
            }

            encoder_reading_t *reading = &s_encoder_readings[config->unit];

            // Read current hardware counter value
            int16_t hw_count = 0;
            pcnt_get_counter_value(config->unit, &hw_count);

            // Calculate delta since last poll
            int32_t delta = (int32_t)hw_count - reading->count;
            if (delta != 0) {
                const char *unit_name = (config->unit == WAVEX_ENCODER_PCNT_UNIT)
                                            ? "Main Encoder"
                                            : "PCNT1 Encoder (PEC11R quadrature)";
                ESP_LOGI(TAG,
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
                pcnt_counter_clear(config->unit);
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
                ESP_LOGE(TAG, "Failed to initialize PCNT unit %d", config->unit);
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

esp_err_t pcnt_get_reading(pcnt_unit_t unit, encoder_reading_t *reading) {
    if (unit >= PCNT_UNIT_MAX || reading == NULL) {
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

esp_err_t pcnt_reset_counter(pcnt_unit_t unit) {
    if (unit >= PCNT_UNIT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = pcnt_counter_clear(unit);
    if (ret == ESP_OK) {
        s_encoder_readings[unit].count = 0;
        s_encoder_readings[unit].prev_count = 0;
        s_encoder_readings[unit].delta = 0;
    }
    return ret;
}

esp_err_t pcnt_get_raw_count(pcnt_unit_t unit, int16_t *count) {
    if (unit >= PCNT_UNIT_MAX || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return pcnt_get_counter_value(unit, count);
}

int32_t pcnt_consume_delta(pcnt_unit_t unit) {
    if (unit >= PCNT_UNIT_MAX) {
        return 0;
    }
    // Fetch and clear atomically with interrupts disabled to avoid ISR race.
    uint32_t prev_level = portSET_INTERRUPT_MASK_FROM_ISR();
    int32_t delta = s_encoder_readings[unit].delta;
    s_encoder_readings[unit].delta = 0;
    portCLEAR_INTERRUPT_MASK_FROM_ISR(prev_level);
    return delta;
}
