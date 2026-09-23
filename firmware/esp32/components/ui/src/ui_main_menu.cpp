// WaveX UI Main Menu Implementation
#include "ui/ui_main_menu.h"

#include <esp_log.h>

#include "bsp/esp32_p4_nano.h"
#include "config/hardware_config.h"
#include "inter_mcu.h"
#include "ui/current_track.h"
#include "ui/display_manager.h"
#include "ui/ui_api.h"
#include "ui/ui_cv_cal_page.h"
#include "ui/ui_diagnostics_page.h"
#include "ui/ui_global_lfo_page.h"
#include "ui/ui_instrument_page.h"
#include "ui/ui_play_page.h"
#include "ui/ui_pot_cal_page.h"
#include "ui/ui_sample_browser.h"
#include "ui/ui_sample_edit_page.h"
#include "ui/ui_sample_manager_page.h"
#include "ui/ui_sample_record_page.h"
#include "ui/ui_settings_page.h"
#include "ui/ui_system_info_page.h"
#include "ui/ui_tab_host_page.h"

#include <cstdio>
#include <functional>
#include <string>

static const char* TAG = "UI_MAIN_MENU";

namespace wavex_ui {

namespace {

void applyBrightness(int percent) {
    esp_err_t err = DisplayManager::instance().setBrightness(static_cast<uint8_t>(percent));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "brightness set to %d%% failed: %d", percent, (int)err);
    }
}

std::string formatPercent(int v) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    return std::string(buf);
}

}  // namespace

std::shared_ptr<UIPage> createMainMenu() {
    auto menu = std::make_shared<UIMenuPage>("Main Menu");

    // Each item is a jump to its root group - the same call the panel's jump
    // keys make (panel-controls.md §4.3), so a menu selection and a key press
    // are one code path. The pages come from the factories
    // initNavigationSystem() registers, not from here.
    //
    // Instrument is a tab group too, but one page builds its own tabview
    // rather than a UITabHostPage: its five stages share the Instrument being
    // edited, so the header and status line have to outlive a tab switch. See
    // UIInstrumentPage.
    //
    // Context reads the shared backend snapshots; no page-local mirror.
    // Separator is ASCII "/" and not a middle dot: LVGL's built-in Montserrat
    // tables cover printable ASCII, so U+00B7 renders as a box on the panel.
    static const struct {
        const char* label;
        const char* purpose;
        RootGroup group;
    } kItems[] = {
        {"Sample", "Manage / Browse / Edit / Record", RootGroup::Sample},
        {"Project", "Tracks / Instruments / MIDI", RootGroup::Track},
        {"Mixer", "Level / Pan / Mute / Solo / Master", RootGroup::Mixer},
        {"Instrument", "Sample / Env / Amp / Filter / Mod", RootGroup::Instrument},
        {"Play", "Pads / Keys", RootGroup::Play},
        {"Sequencer", "Steps / Tempo / Swing", RootGroup::Sequencer},
        {"Settings", "Display / Storage / MIDI / System / Calibrate", RootGroup::Settings},
        {"Diagnostics",
         "ESP32 / Daisy / Audio / Link / Storage / MIDI / Panel",
         RootGroup::Diagnostics},
    };
    for (const auto& item: kItems) {
        const RootGroup group = item.group;
        auto open = [group]() {
            ESP_LOGI(TAG, "Opening %s", rootGroupName(group));
            UINavigator::instance().jumpToRoot(group); };

        std::function<std::string()> context;
        std::function<bool()> ok;
        if (group == RootGroup::Play) {
            context = []() {
                char buf[16];
                snprintf(buf, sizeof(buf), "Track %u", trackDisplayNumber(getCurrentTrack()));
                return std::string(buf);
            };
        } else if (group == RootGroup::Sample) {
            context = []() {
                if (!inter_mcu_backend_link_alive())
                    return std::string("Pool unavailable");
                // Menu context runs once per second, only while visible.
                inter_mcu_request_sample_meta_page(0, 1);
                uint16_t count = 0;
                if (!inter_mcu_get_sample_pool_count(&count, 3000))
                    return std::string("Pool: checking");
                char text[32];
                snprintf(text, sizeof(text), "%u resident", count);
                return std::string(text);
            };
        } else if (group == RootGroup::Instrument) {
            context = []() {
                if (!inter_mcu_backend_link_alive())
                    return std::string("Instrument unavailable");
                const auto track = getCurrentTrack();
                inter_mcu_request_track_binding(track);
                WaveX::Protocol::TrackBindingMessage binding;
                if (!inter_mcu_get_track_binding(track, &binding, 3000))
                    return std::string("Instrument: checking");
                char text[64];
                const char* name = binding.state == WaveX::Protocol::TRACK_BINDING_EMPTY ? "Empty"
                                   : binding.state == WaveX::Protocol::TRACK_BINDING_LOADING
                                       ? "Loading"
                                   : binding.name[0] ? binding.name
                                                     : "Unnamed";
                snprintf(text, sizeof(text), "Track %u / %.23s", trackDisplayNumber(track), name);
                return std::string(text);
            };
        } else if (group == RootGroup::Diagnostics) {
            // The one piece of root-level state worth seeing without opening
            // anything: a dead link makes every other page lie quietly.
            context = []() {
                return std::string(inter_mcu_backend_link_alive() ? "link OK" : "link down");
            };
            // Not hb.valid: that latches true on the first heartbeat and never
            // clears, so the dot sat on green from boot regardless of whether
            // the Daisy was still talking. It has to go red when the beacons
            // stop or it is not an indicator, just decoration.
            ok = []() { return inter_mcu_backend_link_alive(); };
        }

        menu->addItem(item.label, item.purpose, std::move(context), std::move(ok), open);
    }

    return menu;
}

// Sample: four views of the CURRENT sample, so they are tabs rather than menu
// entries (docs/ui-information-architecture.md §2). Grouping them is also what
// gives the edit page a way to change which sample it edits - selecting in
// Browse or Manage is now that mechanism, closing roadmap 1.5.1 item 7.
std::shared_ptr<UIPage> createSampleGroup() {
    auto group = std::make_shared<UITabHostPage>("Sample");
    group->addTab("Manage", createSampleManagerPage());
    if (auto comm = wavex_ui::uiContext().comm) {
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

// Settings: one tab group in the same chrome as Sample and Diagnostics, with
// CV Calibration folded in as the Calibrate tab
// (docs/ui-information-architecture.md §6).
std::shared_ptr<UIPage> createSettingsGroup() {
    auto group = std::make_shared<UITabHostPage>("Settings");
    group->addTab("Display", createDisplaySettingsPage());
    group->addTab("Storage", createStorageSettingsPage());
    group->addTab("MIDI", createMidiSettingsPage());
    group->addTab("System", createSystemInfoPage());
    group->addTab("Calibrate", createCvCalPage());
    group->addTab("Pots", createPotCalPage());
    group->addTab("Global LFO", createGlobalLfoPage());
    return group;
}

std::shared_ptr<UIPage> createDisplaySettingsPage() {
    auto page = std::make_shared<UISettingsPage>("Display");

    // Brightness is real: the Waveshare panel's backlight driver is an I2C
    // device and the BSP exposes bsp_display_brightness_set(). display_manager
    // drives it to 100% once at start-up so this row's initial value is the
    // truth rather than a guess - the BSP has no getter.
    //
    // The write is a 2-byte I2C transmit at 100 kHz with a 50 ms timeout, so
    // it is *bounded* - the property that matters, and not the kind of
    // open-ended block that has frozen this display before. Its actual cost on
    // the UI task is unmeasured; it happens at the rate a human turns an
    // encoder, and the sweep is on the bench list in roadmap
    // § Outstanding hardware verification.
    page->addSetting("Brightness", 100, 10, 100, applyBrightness, formatPercent);
    page->setDesc("Brightness", "Backlight PWM / 10-100");

    // Contrast is gone rather than stubbed: this is a MIPI-DSI panel driven by
    // an HX8394 with no contrast control to offer, so the row could never do
    // anything. A control that cannot exist is not a TODO.
    page->addInfo("Screen blanking", "after 5 min; touch/button/encoder wakes");
    page->addUnimplemented("Rotation", "not implemented - fixed landscape");
    page->addUnimplemented("Save on power-off", "not implemented - settings reset at boot");

    return page;
}

}  // namespace wavex_ui
