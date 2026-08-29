#include <stdio.h>

#include "wavex_application.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#define ESP_LOGI(TAG, FMT, ...) ((void)0)
#define ESP_LOGE(TAG, FMT, ...) ((void)0)
#endif

static const char *TAG = "WaveX-ESP32";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "app_main starting");

    // Create and initialize the application
    WaveX::WaveXApplication app;

    if (!app.initialize()) {
        // Restart rather than return. `app` is stack-local, so returning runs
        // ~ApplicationContext and frees the StatisticsManager and PacketRouter
        // - but initialize() starts the UART link task before the step that
        // can fail, and that task reaches both through file-scope pointers
        // (inter_mcu.cpp, esp_uart_link.cpp). The next frame off the wire
        // would then be a use-after-free on freed heap. A reboot loop is a
        // visible failure; a silently corrupted one is not.
        ESP_LOGE(TAG, "Failed to initialize WaveX application - restarting");
#ifdef ESP_PLATFORM
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));  // let the log ring drain to the console
        esp_restart();
#else
        return;
#endif
    }

    // Run the main application loop (should not return)
    app.run();
}
