// WaveX UI Main Menu Page
#pragma once

#include "ui_menu_page.h"
#include "ui_navigator.h"

#include <memory>

namespace wavex_ui {

/**
 * @brief Main menu page factory functions
 *
 * Builds the structure `docs/ui-information-architecture.md` §1 specifies:
 * Main Menu -> Play, Sample, Voice, Settings, Diagnostics.
 *
 * Sample, Voice, Settings and Diagnostics are all tab groups. §2 gives two
 * independent reasons for that, either sufficient: the children share a
 * subject (Sample, Voice), or they are single screens small enough that a push
 * and a pop each is more navigation than they are worth (Settings). Settings
 * was originally specified as a list on the first rule alone; §2 was rewritten
 * when the second reason turned out to matter more for it.
 *
 * The old `Modulation` entry is gone - its three items were logging stubs, and
 * modulation is a property of a voice rather than a peer of one, so it lives
 * as Voice's Mod tab.
 */

/**
 * @brief Create the main menu page
 */
std::shared_ptr<UIPage> createMainMenu();

/**
 * @brief Create the Sample tab group (Manage / Browse / Edit / Record)
 */
std::shared_ptr<UIPage> createSampleGroup();

/**
 * @brief Create the Settings tab group (Display / Storage / MIDI / System /
 *        Calibrate).
 *
 * Tabs rather than a menu list: five short screens, none deep enough to be
 * worth a push/pop each, and CV Calibration - previously a list entry two
 * levels down - is one of them.
 */
std::shared_ptr<UIPage> createSettingsGroup();

/**
 * @brief Create the display settings page (Settings ▸ Display)
 */
std::shared_ptr<UIPage> createDisplaySettingsPage();

/**
 * @brief Create the storage settings page (Settings ▸ Storage)
 */
std::shared_ptr<UIPage> createStorageSettingsPage();

/**
 * @brief Create the MIDI settings page (Settings ▸ MIDI)
 */
std::shared_ptr<UIPage> createMidiSettingsPage();

}  // namespace wavex_ui
