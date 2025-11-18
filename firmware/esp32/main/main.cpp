#include <stdio.h>

#include "wavex_application.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(TAG, FMT, ...) ((void)0)
#define ESP_LOGE(TAG, FMT, ...) ((void)0)
#endif

static const char *TAG = "WaveX-ESP32";

extern "C" void app_main(void) {
    // THIS IS THE MOST IMPORTANT DEBUG MESSAGE. IF YOU DON'T SEE THIS, THE APP ISN'T STARTING.
    ESP_LOGE(TAG, "!!!!!!!!!! APP_MAIN HAS STARTED !!!!!!!!!!");

    // Immediate debug output to confirm we reach app_main
    printf("*** EARLY DEBUG: app_main() reached! ***\n");
    fflush(stdout);

    // Create and initialize the application
    WaveX::WaveXApplication app;

    if (!app.initialize()) {
        ESP_LOGE(TAG, "Failed to initialize WaveX application");
        return;
    }

    // Run the main application loop (should not return)
    app.run();
}
