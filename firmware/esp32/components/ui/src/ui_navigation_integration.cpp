// WaveX UI Navigation Integration Implementation
#include "ui/ui_navigation_integration.h"

#include <esp_log.h>

#include "ui/ui_diagnostics_page.h"
#include "ui/ui_instrument_page.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_play_page.h"

static const char* TAG = "UI_NAV_INTEGRATION";

namespace wavex_ui {

void initNavigationSystem() {
    ESP_LOGI(TAG, "Initializing navigation system");

    // What a jump to each root group pushes - from the main menu or from the
    // panel's jump keys. Track and Mixer are deliberately absent: they have
    // keys before they have pages (Phase 2.5), and jumpToRoot() refuses them
    // until a page is registered here.
    auto& nav = UINavigator::instance();
    nav.setRootGroupFactory(RootGroup::Sample, createSampleGroup);
    nav.setRootGroupFactory(RootGroup::Instrument, createInstrumentPage);
    nav.setRootGroupFactory(RootGroup::Play, createPlayPage);
    nav.setRootGroupFactory(RootGroup::Settings, createSettingsGroup);
    nav.setRootGroupFactory(RootGroup::Diagnostics, createDiagnosticsPage);

    auto mainMenu = createMainMenu();
    nav.push(mainMenu);

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
