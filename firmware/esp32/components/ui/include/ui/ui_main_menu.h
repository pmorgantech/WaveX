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
 * Main Menu -> Sample, Voice, Play, Settings, Diagnostics. Sample and Voice
 * are tab groups (§2: tabs when the children share a subject); Settings is a
 * list, because its children share nothing but the word.
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

}  // namespace wavex_ui
