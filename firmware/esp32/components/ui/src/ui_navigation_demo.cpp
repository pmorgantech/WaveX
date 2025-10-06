// WaveX UI Navigation System Demo
#include "ui/ui_navigation_integration.h"
#include "ui/ui_api.h"
#include <esp_log.h>

static const char* TAG = "UI_NAV_DEMO";

namespace wavex_ui {

/**
 * @brief Demo function showing how to integrate the navigation system
 * 
 * This function demonstrates how to replace the existing UI demo
 * with the new navigation system.
 */
void ui_init_navigation_demo() {
    ESP_LOGI(TAG, "Starting navigation system demo");
    
    // Initialize the navigation system with main menu
    initNavigationSystem();
    
    // Set the navigation context as active for input handling
    InputDispatcher::instance().setActiveContext(createNavigationContext());
    
    ESP_LOGI(TAG, "Navigation demo initialized successfully");
    ESP_LOGI(TAG, "Use encoder to navigate menus, buttons to select items");
    ESP_LOGI(TAG, "Softkeys provide context-sensitive actions");
}

} // namespace wavex_ui
