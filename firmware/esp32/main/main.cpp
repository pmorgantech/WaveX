#include <stdio.h>
#include <inttypes.h>
#include "../../shared/config/link_config.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#else
#include <stdio.h>
#define ESP_LOGI(TAG, FMT, ...) ((void)0)
#define ESP_LOGE(TAG, FMT, ...) ((void)0)
static inline unsigned int esp_get_free_heap_size() { return 0U; }
static inline long long esp_timer_get_time() { return 0; }
typedef int esp_err_t;
#define ESP_OK 0
#endif

// Include WaveX configuration
#include "config.h"

// esp_err.h is brought in transitively by ESP-IDF headers when building on target
#include "version.h"
#include "inter_mcu.h"
#include "ui_task.h"

// Bring in the SPI link API
#if WAVEX_SPI_LINK_ENABLED
#include "links/spi_link.h"
#endif

static const char *TAG = "WaveX-ESP32";

extern "C" void app_main(void)
{   
    // THIS IS THE MOST IMPORTANT DEBUG MESSAGE. IF YOU DON'T SEE THIS, THE APP ISN'T STARTING.
    ESP_LOGE(TAG, "!!!!!!!!!! APP_MAIN HAS STARTED !!!!!!!!!!");

    // Immediate debug output to confirm we reach app_main
    printf("*** EARLY DEBUG: app_main() reached! ***\n");
    fflush(stdout);
    
    ESP_LOGI(TAG, "=== WaveX ESP32 Frontend Starting ===");
    ESP_LOGI(TAG, "DEBUG: Checkpoint 1 - Basic startup");
    
    ESP_LOGI(TAG, "Version: %s", WAVEX_FRONTEND_VERSION_STRING);
    ESP_LOGI(TAG, "Built: %s %s", WAVEX_COMPILE_DATE, WAVEX_COMPILE_TIME);
    ESP_LOGI(TAG, "DEBUG: Checkpoint 2 - Version info logged");
    
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "DEBUG: Checkpoint 3 - Heap info logged");
    
    ESP_LOGI(TAG, "DEBUG: Checkpoint 4 - About to initialize inter-MCU");
    
    // Initialize inter-MCU with regular SPI (non-HD mode)
    ESP_LOGI(TAG, "DEBUG: Checkpoint 5 - Starting inter-MCU with regular SPI mode");
    esp_err_t inter_mcu_result = inter_mcu_init();
    if (inter_mcu_result == ESP_OK) {
        ESP_LOGI(TAG, "DEBUG: Checkpoint 5 - Inter-MCU initialization completed successfully");
        
        // Start inter-MCU communication (this starts the SPI link)
        esp_err_t start_result = inter_mcu_start();
        if (start_result == ESP_OK) {
            ESP_LOGI(TAG, "DEBUG: Checkpoint 5a - Inter-MCU communication started successfully");
        } else {
            ESP_LOGE(TAG, "DEBUG: Checkpoint 5a - Inter-MCU start FAILED: %s", esp_err_to_name(start_result));
        }
    } else {
        ESP_LOGE(TAG, "DEBUG: Checkpoint 5 - Inter-MCU initialization FAILED: %s", esp_err_to_name(inter_mcu_result));
    }

    ESP_LOGI(TAG, "DEBUG: Checkpoint 6 - About to delay for UI start");
    
    // Delay UI start slightly to avoid contention with SPI bus init
    #ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "DEBUG: Checkpoint 7 - Delay completed");
    #else
    // Fallback for non-ESP builds
    ESP_LOGI(TAG, "DEBUG: Checkpoint 7 - Non-ESP build (no delay)");
    #endif
    
    ESP_LOGI(TAG, "DEBUG: Checkpoint 8 - About to check UI config");
    
    // Start UI task with minimal version to test display initialization
    #if WAVEX_LCD_DISPLAY_ENABLED && (WAVEX_LCD_DISPLAY_TYPE == 1)
    ESP_LOGI(TAG, "DEBUG: Checkpoint 9 - Starting minimal UI task for testing");
    esp_err_t ui_result = wavex_ui_task_start();
    if (ui_result == ESP_OK) {
        ESP_LOGI(TAG, "DEBUG: Checkpoint 10 - Minimal UI task started successfully");
    } else {
        ESP_LOGE(TAG, "DEBUG: Checkpoint 10 - Minimal UI task start FAILED: %s", esp_err_to_name(ui_result));
    }
    #else
    ESP_LOGI(TAG, "MIPI DSI UI disabled for this configuration.");
    ESP_LOGI(TAG, "DEBUG: Checkpoint 10 - UI disabled");
    #endif

    // Periodically probe Daisy status via SPI even if IRQ line is not wired
    int probe_counter = 0;
    
    ESP_LOGI(TAG, "DEBUG: Checkpoint 11 - About to log initialization complete");
    ESP_LOGI(TAG, "WaveX ESP32 Frontend Initialized");
    ESP_LOGI(TAG, "About menu available under System -> About");
    ESP_LOGI(TAG, "DEBUG: Checkpoint 12 - Entering main loop");
    
    // Main application loop - reduced logging to prevent memory issues
    int loop_counter = 0;
    ESP_LOGI(TAG, "Main loop started - reduced logging mode");

    while (1) {
        loop_counter++;

        // Monitor system status - less frequent logging
        static int last_heap_log = 0;
        int current_time = (int)(esp_timer_get_time() / 1000000);

        if (current_time - last_heap_log >= 60) { // Log every 60 seconds
            ESP_LOGI(TAG, "System status - Loop: %d, Free heap: %" PRIu32 " bytes",
                     loop_counter, esp_get_free_heap_size());
            last_heap_log = current_time;
        }

        #if WAVEX_SPI_LINK_ENABLED
        // Process SPI link without excessive logging
        // SPI operations are handled by inter_mcu
        #endif

        #ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(1000));  // 2 second loop - slower to reduce load
        #else
        // Fallback for non-ESP builds
        #endif
    }
} 