// WaveX UI Main Menu Implementation
#include "ui/ui_main_menu.h"

#include <esp_log.h>

#include "ui/ui_sample_browser.h"
#include "ui/ui_sample_detail.h"
#include "ui/ui_settings_page.h"

static const char* TAG = "UI_MAIN_MENU";

namespace wavex_ui {

std::shared_ptr<UIPage> createMainMenu() {
    auto menu = std::make_shared<UIMenuPage>("Main Menu");

    menu->addItem("Sample Browser",
                  []() {
        ESP_LOGI(TAG, "Opening Sample Browser");
        // TODO: Need to inject comm_interface
        // UINavigator::instance().push(createSampleBrowserPage(comm_interface));
    });

    menu->addItem("Edit Sample",
                  []() {
        ESP_LOGI(TAG, "Edit Sample selected");
        // TODO: Implement sample editing functionality
    });

    menu->addItem("Modulation", []() {
        ESP_LOGI(TAG, "Opening Modulation Menu");
        UINavigator::instance().push(createModulationMenu()); });

    menu->addItem("Settings", []() {
        ESP_LOGI(TAG, "Opening Settings Menu");
        UINavigator::instance().push(createSettingsMenu()); });

    menu->addItem("Diagnostics",
                  []() {
        ESP_LOGI(TAG, "Diagnostics selected");
        // TODO: Implement diagnostics functionality
    });

    return menu;
}

std::shared_ptr<UIPage> createModulationMenu() {
    auto menu = std::make_shared<UIMenuPage>("Modulation");

    menu->addItem("LFO 1",
                  []() {
        ESP_LOGI(TAG, "LFO 1 selected");
        // TODO: Implement LFO 1 editor
    });

    menu->addItem("LFO 2",
                  []() {
        ESP_LOGI(TAG, "LFO 2 selected");
        // TODO: Implement LFO 2 editor
    });

    menu->addItem("Envelopes",
                  []() {
        ESP_LOGI(TAG, "Envelopes selected");
        // TODO: Implement envelope editor
    });

    return menu;
}

std::shared_ptr<UIPage> createSettingsMenu() {
    auto menu = std::make_shared<UIMenuPage>("Settings");

    menu->addItem("Display", []() {
        ESP_LOGI(TAG, "Opening Display Settings");
        UINavigator::instance().push(createDisplaySettingsPage()); });

    menu->addItem("MIDI", []() {
        ESP_LOGI(TAG, "Opening MIDI Settings");
        UINavigator::instance().push(createMidiSettingsPage()); });

    menu->addItem("Storage",
                  []() {
        ESP_LOGI(TAG, "Storage settings selected");
        // TODO: Implement storage settings
    });

    menu->addItem("System Info",
                  []() {
        ESP_LOGI(TAG, "System Info selected");
        // TODO: Implement system info display
    });

    return menu;
}

std::shared_ptr<UIPage> createDisplaySettingsPage() {
    auto page = std::make_shared<UISettingsPage>("Display Settings");

    page->addSetting("Brightness",
                     75,
                     0,
                     100,
                     [](int value) {
        ESP_LOGI(TAG, "Brightness set to %d%%", value);
        // TODO: Apply brightness setting
    });

    page->addSetting("Contrast",
                     50,
                     0,
                     100,
                     [](int value) {
        ESP_LOGI(TAG, "Contrast set to %d%%", value);
        // TODO: Apply contrast setting
    });

    page->addSetting("Timeout",
                     30,
                     5,
                     300,
                     [](int value) {
        ESP_LOGI(TAG, "Display timeout set to %d seconds", value);
        // TODO: Apply timeout setting
    });

    return page;
}

std::shared_ptr<UIPage> createMidiSettingsPage() {
    auto page = std::make_shared<UISettingsPage>("MIDI Settings");

    page->addSetting("MIDI Channel",
                     1,
                     1,
                     16,
                     [](int value) {
        ESP_LOGI(TAG, "MIDI Channel set to %d", value);
        // TODO: Apply MIDI channel setting
    });

    page->addSetting("Velocity Curve",
                     2,
                     1,
                     4,
                     [](int value) {
        ESP_LOGI(TAG, "Velocity Curve set to %d", value);
        // TODO: Apply velocity curve setting
    });

    page->addSetting("Clock Source",
                     1,
                     1,
                     3,
                     [](int value) {
        ESP_LOGI(TAG, "Clock Source set to %d", value);
        // TODO: Apply clock source setting
    });

    return page;
}

}  // namespace wavex_ui
