// WaveX UI Main Menu Implementation
#include "ui/ui_main_menu.h"

#include <esp_log.h>

#include "bsp/esp32_p4_nano.h"
#include "config/hardware_config.h"
#include "midi_task.h"
#include "ui/display_manager.h"
#include "ui/ui_api.h"
#include "ui/ui_cv_cal_page.h"
#include "ui/ui_diagnostics_page.h"
#include "ui/ui_play_page.h"
#include "ui/ui_sample_browser.h"
#include "ui/ui_sample_edit_page.h"
#include "ui/ui_sample_manager_page.h"
#include "ui/ui_sample_record_page.h"
#include "ui/ui_settings_page.h"
#include "ui/ui_system_info_page.h"
#include "ui/ui_tab_host_page.h"
#include "ui/ui_voice_page.h"

#include <cstdio>
#include <string>

static const char* TAG = "UI_MAIN_MENU";

// USB MIDI input needs both flags, and the inner one is only *defined* when
// the outer is on (hardware_config.h), so the pair has to be resolved by the
// preprocessor rather than tested as a C++ expression. This is the same
// condition usb_midi_task.cpp compiles itself out on.
#if WAVEX_ESP_USB_MIDI_ENABLED && WAVEX_USB_MIDI_INPUT_ENABLED
#define WAVEX_UI_USB_MIDI_IN 1
#else
#define WAVEX_UI_USB_MIDI_IN 0
#endif

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

std::string formatMidiChannel(int v) {
    if (v == 0) {
        return std::string("Omni");
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    return std::string(buf);
}

}  // namespace

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
        ESP_LOGI(TAG, "Opening Settings");
        UINavigator::instance().push(createSettingsGroup()); });

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

    // Contrast is gone rather than stubbed: this is a MIPI-DSI panel driven by
    // an HX8394 with no contrast control to offer, so the row could never do
    // anything. A control that cannot exist is not a TODO.
    page->addInfo("Screen blanking", "after 5 min; touch/button/encoder wakes");
    page->addUnimplemented("Rotation", "not implemented - fixed landscape");
    page->addUnimplemented("Save on power-off", "not implemented - settings reset at boot");

    return page;
}

std::shared_ptr<UIPage> createStorageSettingsPage() {
    auto page = std::make_shared<UISettingsPage>("Storage");

    // There is genuinely nothing to configure here yet: the SD card belongs to
    // the Daisy, which mounts it itself, and every number worth seeing is
    // already on the Diagnostics ▸ Storage tab. Saying that is more useful
    // than three sliders that log and return.
    page->addInfo("Storage settings", "none yet");
    page->addInfo("SD card status", "see Diagnostics > Storage");
    page->addUnimplemented("Format SD card", "not implemented - format the card on a computer");
    page->addUnimplemented("Sample folder", "not implemented - the Daisy chooses the path");

    return page;
}

std::shared_ptr<UIPage> createMidiSettingsPage() {
    auto page = std::make_shared<UISettingsPage>("MIDI");

    // Receive channel is real: midi_forward_event() applies it to both the DIN
    // and USB readers. 0 is Omni.
    page->addSetting(
        "Receive channel",
        midi_get_input_channel(),
        0,
        16,
        [](int value) { midi_set_input_channel(value); },
        formatMidiChannel);

    page->addInfo("DIN MIDI in", WAVEX_ESP_DIN_MIDI_ENABLED ? "enabled" : "disabled in build");
    page->addInfo("USB MIDI in", WAVEX_UI_USB_MIDI_IN ? "enabled" : "disabled in build");

    // Everything below is named because it is the first thing anyone looks for
    // on a MIDI settings page, and finding nothing is ambiguous in a way that
    // "not implemented" is not.
    page->addUnimplemented("Velocity curve", "not implemented - velocity passes through unchanged");
    page->addUnimplemented("Clock source", "not implemented - MIDI clock is not received yet");
    page->addUnimplemented("MIDI out", "not implemented - no output port is driven");
    page->addUnimplemented("CC mapping", "not implemented - incoming CCs are discarded");
    page->addUnimplemented("Save on power-off", "not implemented - channel resets to Omni at boot");

    return page;
}

}  // namespace wavex_ui
