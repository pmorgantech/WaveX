#include "ui_lgfx/screensaver.h"
#include "ui_lgfx/lgfx_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "SCREENSAVER";

// Screensaver state
static struct {
    int timeout_minutes;
    int saved_brightness;
    bool is_active;
    esp_timer_handle_t timer;
    SemaphoreHandle_t mutex;
} screensaver_state = {0};

// Timer callback - turns off backlight
static void screensaver_timer_callback(void* arg) {
    if (xSemaphoreTake(screensaver_state.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (screensaver_state.timeout_minutes > 0 && !screensaver_state.is_active) {
            // Save current brightness and turn off backlight
            screensaver_state.saved_brightness = 179; // Default 70% if we can't read current
            screensaver_state.is_active = true;
            lgfx_device_set_brightness(0);
            ESP_LOGI(TAG, "Screensaver activated - backlight off");
        }
        xSemaphoreGive(screensaver_state.mutex);
    }
}

extern "C" {

void screensaver_init(void) {
    screensaver_state.mutex = xSemaphoreCreateMutex();
    screensaver_state.timeout_minutes = 5; // Default 5 minutes
    screensaver_state.is_active = false;
    screensaver_state.saved_brightness = 179; // Default 70%
    
    // Create timer
    esp_timer_create_args_t timer_args = {
        .callback = screensaver_timer_callback,
        .arg = NULL,
        .name = "screensaver"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &screensaver_state.timer));
    
    ESP_LOGI(TAG, "Screensaver initialized with %d minute timeout", screensaver_state.timeout_minutes);
}

void screensaver_set_timeout(int minutes) {
    if (xSemaphoreTake(screensaver_state.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Stop existing timer
        esp_timer_stop(screensaver_state.timer);
        
        screensaver_state.timeout_minutes = minutes;
        
        if (minutes > 0) {
            // Start new timer
            uint64_t timeout_us = (uint64_t)minutes * 60 * 1000000; // Convert to microseconds
            ESP_ERROR_CHECK(esp_timer_start_once(screensaver_state.timer, timeout_us));
            ESP_LOGI(TAG, "Screensaver timeout set to %d minutes", minutes);
        } else {
            ESP_LOGI(TAG, "Screensaver disabled");
        }
        
        xSemaphoreGive(screensaver_state.mutex);
    }
}

void screensaver_reset_timer(void) {
    if (xSemaphoreTake(screensaver_state.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // If screensaver is active, wake up
        if (screensaver_state.is_active) {
            lgfx_device_set_brightness(screensaver_state.saved_brightness);
            screensaver_state.is_active = false;
            ESP_LOGI(TAG, "Screensaver deactivated - backlight restored to %d", screensaver_state.saved_brightness);
        }
        
        // Reset timer if enabled
        if (screensaver_state.timeout_minutes > 0) {
            esp_timer_stop(screensaver_state.timer);
            uint64_t timeout_us = (uint64_t)screensaver_state.timeout_minutes * 60 * 1000000;
            ESP_ERROR_CHECK(esp_timer_start_once(screensaver_state.timer, timeout_us));
        }
        
        xSemaphoreGive(screensaver_state.mutex);
    }
}

} // extern "C"
