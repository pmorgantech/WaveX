#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ui_main.h"
#include "version.h"
#include "hardware_pins.h"
#include "inter_mcu.h"
#include "esp_timer.h"  // Add this missing include

static const char *TAG = "WaveX-ESP32";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== WaveX ESP32 Frontend Starting ===");
    ESP_LOGI(TAG, "Version: %s", WAVEX_FRONTEND_VERSION_STRING);
    ESP_LOGI(TAG, "Built: %s %s", WAVEX_COMPILE_DATE, WAVEX_COMPILE_TIME);
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
    
    ESP_LOGI(TAG, "Initializing hardware...");
    
    // Initialize hardware configuration
    esp_err_t ret = wavex_hardware_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Hardware initialization failed: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "Hardware initialization completed successfully");
    
    // Initialize inter-MCU communication
    if (inter_mcu_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize inter-MCU SPI");
        return;
    }
    
    // Initialize LVGL and UI
    wavex_ui_init();
    
    ESP_LOGI(TAG, "WaveX ESP32 Frontend Initialized");
    ESP_LOGI(TAG, "About menu available under System -> About");
    
    // Main application loop
    while (1) {
        // Monitor system status
        static int last_heap_log = 0;
        int current_time = (int)(esp_timer_get_time() / 1000000); // Fix: cast to int and add parentheses
        
        if (current_time - last_heap_log >= 30) { // Log every 30 seconds
            ESP_LOGI(TAG, "System running - Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
            last_heap_log = current_time;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second loop
    }
} 