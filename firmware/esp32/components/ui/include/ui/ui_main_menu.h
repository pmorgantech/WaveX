// WaveX UI Main Menu Page
#pragma once

#include "ui_menu_page.h"
#include "ui_navigator.h"
#include <memory>

namespace wavex_ui {

/**
 * @brief Main menu page factory functions
 * 
 * Creates the hierarchical menu structure as described in the document:
 * Main Menu -> Sample Browser, Edit Sample, Modulation, Settings, Diagnostics
 */

/**
 * @brief Create the main menu page
 */
std::shared_ptr<UIPage> createMainMenu();

/**
 * @brief Create the modulation submenu
 */
std::shared_ptr<UIPage> createModulationMenu();

/**
 * @brief Create the settings submenu
 */
std::shared_ptr<UIPage> createSettingsMenu();

/**
 * @brief Create the display settings page
 */
std::shared_ptr<UIPage> createDisplaySettingsPage();

/**
 * @brief Create the MIDI settings page
 */
std::shared_ptr<UIPage> createMidiSettingsPage();

} // namespace wavex_ui
