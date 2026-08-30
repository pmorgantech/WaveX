// WaveX UI Navigation Integration Implementation
#include "ui/ui_navigation_integration.h"

#include <esp_log.h>

#include "ui/ui_main_menu.h"

static const char* TAG = "UI_NAV_INTEGRATION";

namespace wavex_ui {

void initNavigationSystem() {
    ESP_LOGI(TAG, "Initializing navigation system");

    // Create and push the main menu as the root page
    auto mainMenu = createMainMenu();
    UINavigator::instance().push(mainMenu);

    ESP_LOGI(TAG, "Navigation system initialized with main menu");
}

void handleNavigationInput(const InputEvent& evt) {
    auto activePage = UINavigator::instance().active();
    if (activePage) {
        activePage->onInput(evt);
    }
}

std::shared_ptr<UIContext> createNavigationContext() {
    return std::make_shared<UIContext>("Navigation", handleNavigationInput);
}

bool isNavigationActive() {
    return UINavigator::instance().active() != nullptr;
}

}  // namespace wavex_ui
