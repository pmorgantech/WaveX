// WaveX UI Main Menu Implementation
#include "ui/ui_main_menu.h"

#include <esp_log.h>

#include "ui/ui_api.h"
#include "ui/ui_cv_cal_page.h"
#include "ui/ui_diagnostics_page.h"
#include "ui/ui_play_page.h"
#include "ui/ui_sample_browser.h"
#include "ui/ui_sample_detail.h"
#include "ui/ui_sample_edit_page.h"
#include "ui/ui_sample_manager_page.h"
#include "ui/ui_sample_record_page.h"
#include "ui/ui_settings_page.h"
#include "ui/ui_tab_host_page.h"
#include "ui/ui_voice_page.h"

static const char* TAG = "UI_MAIN_MENU";

namespace wavex_ui {

std::shared_ptr<UIPage> createMainMenu() {
    auto menu = std::make_shared<UIMenuPage>("Main Menu");

    menu->addItem("Sample", []() {
        ESP_LOGI(TAG, "Opening Sample");
        UINavigator::instance().push(createSampleGroup()); });

    // Voice is a tab group too, but one page builds its own tabview rather than
    // a UITabHostPage: its five stages share the voice being edited, so the
    // header and status line have to outlive a tab switch. See UIVoicePage.
    menu->addItem("Voice", []() {
        ESP_LOGI(TAG, "Opening Voice");
        UINavigator::instance().push(createVoicePage()); });

    menu->addItem("Play", []() {
        ESP_LOGI(TAG, "Opening Play");
        UINavigator::instance().push(createPlayPage()); });

    menu->addItem("Settings", []() {
        ESP_LOGI(TAG, "Opening Settings Menu");
        UINavigator::instance().push(createSettingsMenu()); });

    menu->addItem("Diagnostics", []() {
        ESP_LOGI(TAG, "Diagnostics selected");
        UINavigator::instance().push(createDiagnosticsPage()); });

    return menu;
}

// Sample: four views of the CURRENT sample, so they are tabs rather than menu
// entries (docs/ui-information-architecture.md §2). Grouping them is also what
// gives the edit page a way to change which sample it edits - selecting in
// Browse or Manage is now that mechanism, closing roadmap 1.5.1 item 7.
std::shared_ptr<UIPage> createSampleGroup() {
    auto group = std::make_shared<UITabHostPage>("Sample");
    group->addTab("Manage", createSampleManagerPage());
    if (auto comm = wavex_ui::ui_get_comm_interface()) {
        group->addTab("Browse", createSampleBrowserPage(*comm));
    } else {
        // Browse needs the link; without it the tab would build an empty list
        // and look broken rather than absent.
        ESP_LOGE(TAG, "No comm interface - Browse tab omitted");
    }
    group->addTab("Edit", createSampleEditPage());
    group->addTab("Record", createSampleRecordPage());
    return group;
}

std::shared_ptr<UIPage> createSettingsMenu() {
    auto menu = std::make_shared<UIMenuPage>("Settings");

    menu->addItem("CV Calibration", []() {
        ESP_LOGI(TAG, "CV Calibration selected");
        UINavigator::instance().push(createCvCalPage()); });

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
