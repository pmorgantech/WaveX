#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ui_main.h"
#include "version.h"
#include "hardware_pins.h"

static const char *TAG = "WaveX-ESP32";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "WaveX ESP32 Frontend Starting...");
    ESP_LOGI(TAG, "Version: %s", WAVEX_FRONTEND_VERSION_STRING);
    ESP_LOGI(TAG, "Built: %s %s", WAVEX_COMPILE_DATE, WAVEX_COMPILE_TIME);
    
    // Initialize UI system (includes LVGL initialization)
    ESP_LOGI(TAG, "Initializing UI system...");
    ui_main_init();
    
    // TODO: Initialize SPI communication with Daisy
    // TODO: Initialize SD card
    // TODO: Initialize USB MIDI
    // TODO: Initialize touch controller
    // TODO: Initialize display drivers
    
    ESP_LOGI(TAG, "WaveX ESP32 Frontend Initialized");
    ESP_LOGI(TAG, "About menu available under System -> About");
    
    // esp_lvgl_port manages LVGL timer automatically in background task
    // Main thread can now focus on application logic
    while (1) {
        // TODO: Add main application logic here
        // - SPI communication with Daisy
        // - SD card operations 
        // - USB MIDI handling
        // - System monitoring
        
        // Monitor backlight state
        static int last_bl_state = -1;
        int current_bl_state = gpio_get_level(WAVEX_LCD_GPIO_BL);
        if (current_bl_state != last_bl_state) {
            ESP_LOGI("WaveX-ESP32", "Backlight state changed: %d -> %d", last_bl_state, current_bl_state);
            last_bl_state = current_bl_state;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms loop for application tasks
    }
} 