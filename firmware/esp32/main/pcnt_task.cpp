/**
 * @file pcnt_task.cpp
 * @brief PCNT (Pulse Counter) Task Implementation for ESP32 Encoder Handling
 *
 * This module provides PCNT-based encoder reading functionality for both
 * the main encoder (PCNT_UNIT_0) and additional PCNT unit (PCNT_UNIT_1).
 */

#include "pcnt_task.h"
#include "esp_log.h"
#include "driver/pcnt.h"
#include "../../shared/config/pin_config.h"
#include "../../shared/config/hardware_config.h"

// For interrupt handling
#include "esp_intr_alloc.h"
#include "soc/pcnt_reg.h"

static const char *TAG = "PCNT_TASK";

// PCNT unit configurations
static wavex_pcnt_config_t s_pcnt_configs[] = {
    // Main encoder (PCNT_UNIT_0)
    {
        .unit = WAVEX_ENCODER_PCNT_UNIT,
        .channel_a = WAVEX_ENCODER_PCNT_CH_A,
        .channel_b = WAVEX_ENCODER_PCNT_CH_B,
        .gpio_a = WAVEX_ESP_ENCODER_A,
        .gpio_b = WAVEX_ESP_ENCODER_B,
        .filter_value = WAVEX_ENCODER_FILTER_VALUE,
        .enabled = WAVEX_ESP_ENCODER_PCNT_ENABLED
    },
    // Additional PCNT unit (PCNT_UNIT_1)
    {
        .unit = WAVEX_PCNT1_UNIT,
        .channel_a = WAVEX_PCNT1_CH_A,
        .channel_b = WAVEX_PCNT1_CH_B,
        .gpio_a = WAVEX_ESP_PCNT1_A,
        .gpio_b = WAVEX_ESP_PCNT1_B,
        .filter_value = WAVEX_PCNT1_FILTER_VALUE,
        .enabled = WAVEX_ESP_PCNT1_ENABLED
    }
};

#define PCNT_CONFIG_COUNT (sizeof(s_pcnt_configs) / sizeof(wavex_pcnt_config_t))

// Encoder readings storage
static encoder_reading_t s_encoder_readings[PCNT_UNIT_MAX] = {0};

// Task handle
static TaskHandle_t s_pcnt_task_handle = NULL;

/**
 * @brief Initialize a single PCNT unit
 */
static esp_err_t pcnt_init_unit(const wavex_pcnt_config_t *config) {
    ESP_LOGI(TAG, "Initializing PCNT unit %d (GPIO A:%d, B:%d)",
             config->unit, config->gpio_a, config->gpio_b);

    // Configure PCNT unit
    pcnt_config_t esp_pcnt_config = {
        .pulse_gpio_num = config->gpio_a,
        .ctrl_gpio_num = config->gpio_b,
        .lctrl_mode = PCNT_MODE_REVERSE,
        .hctrl_mode = PCNT_MODE_KEEP,
        .pos_mode = PCNT_COUNT_INC,
        .neg_mode = PCNT_COUNT_DEC,
        .counter_h_lim = INT16_MAX,
        .counter_l_lim = INT16_MIN,
        .unit = config->unit,
        .channel = config->channel_a
    };

    esp_err_t ret = pcnt_unit_config(&esp_pcnt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PCNT unit %d channel A: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    // Configure channel B
    esp_pcnt_config.pulse_gpio_num = config->gpio_b;
    esp_pcnt_config.ctrl_gpio_num = config->gpio_a;
    esp_pcnt_config.channel = config->channel_b;
    esp_pcnt_config.pos_mode = PCNT_COUNT_DEC;
    esp_pcnt_config.neg_mode = PCNT_COUNT_INC;

    ret = pcnt_unit_config(&esp_pcnt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PCNT unit %d channel B: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    // Configure glitch filter
    ret = pcnt_set_filter_value(config->unit, config->filter_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set filter value for PCNT unit %d: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    ret = pcnt_filter_enable(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable filter for PCNT unit %d: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    // Initialize counter
    ret = pcnt_counter_pause(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to pause PCNT unit %d: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    ret = pcnt_counter_clear(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear PCNT unit %d: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    // Start counter
    ret = pcnt_counter_resume(config->unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resume PCNT unit %d: %s",
                 config->unit, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PCNT unit %d initialized successfully", config->unit);
    return ESP_OK;
}

/**
 * @brief PCNT interrupt handler
 */
static void IRAM_ATTR pcnt_isr_handler(void *arg) {
    uint32_t unit_val = (uint32_t)arg;
    pcnt_unit_t unit = (pcnt_unit_t)unit_val;
    encoder_reading_t *reading = &s_encoder_readings[unit];

    // Read hardware counter delta since last clear
    int16_t raw_count = 0;
    pcnt_get_counter_value(unit, &raw_count);

    // Accumulate in software so we can safely clear HW counter for per-step interrupts
    int32_t step_delta = (int32_t)raw_count;
    if (step_delta != 0) {
        reading->prev_count = reading->count;
        reading->count += step_delta;
        reading->delta = step_delta;
    }

    // Clear hardware counter so next pulse will trigger the threshold again
    pcnt_counter_clear(unit);

    // Clear interrupt flags for this unit
    // Note: Legacy PCNT driver may not have pcnt_intr_status_clear
    // The interrupt status is automatically cleared when the ISR runs

    // Optional: Notify task of interrupt (if needed for additional processing)
    // This would require a semaphore or queue
}

/**
 * @brief PCNT monitoring task (interrupt-driven with continuous change logging)
 */
static void pcnt_task(void *pvParameters) {
    ESP_LOGI(TAG, "PCNT monitoring task started (interrupt-driven with continuous logging)");

    // Install ISR service
    esp_err_t ret = pcnt_isr_service_install(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install PCNT ISR service: %s", esp_err_to_name(ret));
        return;
    }

    // Configure interrupts for each enabled PCNT unit
    for (int i = 0; i < PCNT_CONFIG_COUNT; i++) {
        const wavex_pcnt_config_t *config = &s_pcnt_configs[i];
        if (!config->enabled) {
            continue;
        }

        // Configure watch points so every pulse (+/-1) generates an interrupt.
        // We accumulate in software in the ISR and clear the HW counter each time.
        int thresh_pos = 1;
        int thresh_neg = -1;

        pcnt_set_event_value(config->unit, PCNT_EVT_THRES_0, thresh_pos);
        pcnt_event_enable(config->unit, PCNT_EVT_THRES_0);

        pcnt_set_event_value(config->unit, PCNT_EVT_THRES_1, thresh_neg);
        pcnt_event_enable(config->unit, PCNT_EVT_THRES_1);

        // Also enable zero crossing interrupts for full rotation tracking
        pcnt_event_enable(config->unit, PCNT_EVT_ZERO);

        // Enable interrupts for this unit
        pcnt_intr_enable(config->unit);

        // Register ISR handler
        ret = pcnt_isr_handler_add(config->unit, pcnt_isr_handler, (void*)config->unit);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add ISR handler for PCNT unit %d: %s",
                     config->unit, esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "PCNT unit %d interrupt handler installed (thresholds: %d, %d)",
                 config->unit, thresh_pos, thresh_neg);
    }

    while (1) {
        // Monitor for encoder value changes and log them immediately
        for (int i = 0; i < PCNT_CONFIG_COUNT; i++) {
            const wavex_pcnt_config_t *config = &s_pcnt_configs[i];
            if (!config->enabled) {
                continue;
            }

            encoder_reading_t *reading = &s_encoder_readings[config->unit];

            // Check if there's a change since last interrupt-driven update
            if (reading->delta != 0) {
                const char* unit_name = (config->unit == WAVEX_ENCODER_PCNT_UNIT) ? "Main Encoder" : "PCNT1 Encoder";
                ESP_LOGI(TAG, "%s - Count: %" PRId32 ", Delta: %" PRId32,
                         unit_name, reading->count, reading->delta);

                // Clear delta so we don't re-log the same value when idle
                reading->delta = 0;
            }
        }

        // Brief delay to prevent busy-waiting while still being responsive
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
    BaseType_t ret = xTaskCreate(
        pcnt_task,              // Task function
        "pcnt_task",           // Task name
        4096,                  // Stack size
        NULL,                  // Parameters
        5,                     // Priority (higher than UI task)
        &s_pcnt_task_handle    // Task handle
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
