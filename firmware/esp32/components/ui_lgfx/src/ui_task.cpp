#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#else
#include <stdio.h>
#include <stdint.h>
// Minimal shims for non-IDF linting (C++17)
typedef void (*TaskFunction_t)(void*);
static inline void vTaskDelay(uint32_t) {}
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(x) (x)
#endif
static inline int xTaskCreatePinnedToCore(TaskFunction_t, const char*, unsigned int, void*, unsigned int, void*, int) { return 0; }
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#endif
#include "ui_lgfx/lgfx_device.h"
#include "ui_lgfx/ui_task.h"
#include "ui_lgfx/ui_manager.h"
#include "ui_lgfx/screens_main.h"
#include "ui_lgfx/screensaver.h"
#include "inter_mcu.h"


static const char *TAG = "ui_task";

static void ui_task_func(void *arg) {
    ESP_LOGI(TAG, "UI task started (stub)");
    // Conditional LGFX init to avoid conflicts with esp_lcd/LVGL during migration
    // Enable via menuconfig: UI (LovyanGFX) -> Enable LovyanGFX device init
    #if CONFIG_UI_LGFX_ENABLE_INIT
    // Pause inter-MCU RX briefly during LGFX init to avoid bus/DMA contention
    // Start LGFX first; inter-MCU bus bring-up is now deferred until after display init
    inter_mcu_set_suspended(true);
    lgfx_device_init();
    // Now start the inter-MCU SPI stack
    inter_mcu_start();
    inter_mcu_set_suspended(false);
    
    // Initialize screensaver
    screensaver_init();
    
    // Boot into Main Menu
    UIManager::instance().push(new MainMenuScreen());
    // Touch polling and auto-update loop
    ESP_LOGI(TAG, "Touch polling and auto-update started");
    uint32_t tick_count = 0;
    while (true) {
        // Check for touch events
        unsigned short x = 0, y = 0;
        if (lgfx_device_get_touch(&x, &y)) {
            // Reset screensaver timer on any touch
            screensaver_reset_timer();
            
            UIEvent ev{}; ev.type = UIEvent::TOUCH; ev.x = (int)x; ev.y = (int)y;
            UIManager::instance().dispatch(ev);
            ESP_LOGI(TAG, "Touch: %u,%u", (unsigned)x, (unsigned)y);
        }
        
        // Auto-update only screens that need it every 10 ticks (1000ms with 100ms delay)
        tick_count++;
        if (tick_count >= 10) {
            // Only redraw if current screen needs auto-update
            auto* current_screen = UIManager::instance().current();
            if (current_screen && current_screen->needsAutoUpdate()) {
                // Use the screen's updateContent method for efficient partial updates
                current_screen->updateContent();
            }
            tick_count = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms for responsiveness
    }
    #else
    // If LGFX init is disabled, still bring up inter-MCU link here
    inter_mcu_start();
    ESP_LOGI(TAG, "Inter-MCU link started (LGFX init disabled)");
    while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    #endif
}

extern "C" void wavex_ui_task_start(void) {
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreatePinnedToCore(
        ui_task_func,
        "UI_Task",
        4096,
        NULL,
        1,
        NULL,
        0
    );
}


