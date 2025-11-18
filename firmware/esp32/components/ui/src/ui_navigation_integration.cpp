// WaveX UI Navigation Integration Implementation
#include "ui/ui_navigation_integration.h"

#include <esp_log.h>

#include "ui/ui_api.h"
#include "ui/ui_diagnostics_page.h"
#include "ui/ui_sample_browser.h"

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

std::shared_ptr<UIPage> createMainMenu() {
    auto menu = std::make_shared<UIMenuPage>("Main Menu");

    menu->addItem("Sample", []() {
        ESP_LOGI(TAG, "Opening Sample Menu");
        UINavigator::instance().push(createSampleMenu()); });

    menu->addItem("System", []() {
        ESP_LOGI(TAG, "Opening System Menu");
        UINavigator::instance().push(createSystemMenu()); });

    return menu;
}

std::shared_ptr<UIPage> createSampleMenu() {
    auto menu = std::make_shared<UIMenuPage>("Sample Menu");

    menu->addItem("Record",
                  []() {
        ESP_LOGI(TAG, "Record option selected");
        // TODO: Implement record functionality
    });

    menu->addItem("Edit",
                  []() {
        ESP_LOGI(TAG, "Edit option selected");
        // TODO: Implement edit functionality
    });

    menu->addItem("Browser", []() {
        ESP_LOGI(TAG, "Opening Sample Browser page");
        auto comm_interface = wavex_ui::ui_get_comm_interface();
        if (comm_interface) {
            UINavigator::instance().push(createSampleBrowserPage(*comm_interface));
        } else {
            ESP_LOGE(TAG, "No comm interface available for Sample Browser");
        } });

    return menu;
}

std::shared_ptr<UIPage> createSystemMenu() {
    auto menu = std::make_shared<UIMenuPage>("System Menu");

    menu->addItem("Diagnostics", []() {
        ESP_LOGI(TAG, "Opening Diagnostics page");
        UINavigator::instance().push(createDiagnosticsPage()); });

    menu->addItem("Settings",
                  []() {
        ESP_LOGI(TAG, "Settings option selected");
        // TODO: Implement settings functionality
    });

    return menu;
}

}  // namespace wavex_ui
