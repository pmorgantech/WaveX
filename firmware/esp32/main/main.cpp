#include <stdio.h>
#include <inttypes.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
#include <stdio.h>
#define ESP_LOGI(TAG, FMT, ...) ((void)0)
#define ESP_LOGE(TAG, FMT, ...) ((void)0)
static inline unsigned int esp_get_free_heap_size() { return 0U; }
static inline long long esp_timer_get_time() { return 0; }
typedef int esp_err_t;
#define ESP_OK 0
#endif
// esp_err.h is brought in transitively by ESP-IDF headers when building on target
#include "version.h"
#include "inter_mcu.h"
#include "ui_lgfx/ui_task.h"

// Bring in the SPI link API
#if WAVEX_SPI_LINK_ENABLED
#include "links/spi_link.h"
#endif

static const char *TAG = "WaveX-ESP32";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== WaveX ESP32 Frontend Starting ===");
    ESP_LOGI(TAG, "Version: %s", WAVEX_FRONTEND_VERSION_STRING);
    ESP_LOGI(TAG, "Built: %s %s", WAVEX_COMPILE_DATE, WAVEX_COMPILE_TIME);
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
    
    ESP_LOGI(TAG, "Initializing display (LovyanGFX)...");
    
    // Initialize inter-MCU (deferred start; actual SPI bring-up happens in UI task after LGFX init)
    (void)inter_mcu_init();

    // Delay UI start slightly to avoid contention with SPI bus init
    vTaskDelay(pdMS_TO_TICKS(200));
    // Start LovyanGFX-based UI task
    wavex_ui_task_start();

    // Periodically probe Daisy status via SPI even if IRQ line is not wired
    int probe_counter = 0;
    
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

        #if WAVEX_SPI_LINK_ENABLED
        // Note: SPI packet processing is now handled automatically by the link manager
        // and packet processor in the inter-MCU system. No manual processing needed here.
        
        // The link manager automatically processes packets in the background
        // No manual intervention needed
        #endif
        
        vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second loop
    }
} 