// WaveX UI Main Menu Page
#pragma once

#include "ui_menu_page.h"
#include "ui_navigator.h"

#include <memory>

namespace wavex_ui {

/**
 * @brief Top-level navigation, per docs/ui-information-architecture.md §1:
 *        Play, Sample, Voice, Settings, Diagnostics.
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
