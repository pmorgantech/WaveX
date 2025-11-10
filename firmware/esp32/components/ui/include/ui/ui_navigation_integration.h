// WaveX UI Navigation Integration
#pragma once

#include "input_dispatcher.h"
#include "ui_menu_page.h"
#include "ui_navigator.h"

#include <memory>

namespace wavex_ui {

/**
 * @brief Navigation system integration functions
 *
 * Provides integration between the navigation system and existing
 * UI task and input dispatcher.
 */

/**
 * @brief Initialize the navigation system
 *
 * Sets up the navigation stack with the main menu as the root page.
 * Should be called during UI initialization.
 */
void initNavigationSystem();

/**
 * @brief Handle input events through the navigation system
 *
 * Routes input events to the currently active page.
 * Should be called from the input dispatcher.
 *
 * @param evt Input event to handle
 */
void handleNavigationInput(const InputEvent& evt);

/**
 * @brief Get the current navigation context for input handling
 *
 * Creates a UIContext that forwards input to the navigation system.
 *
 * @return UIContext for navigation input handling
 */
std::shared_ptr<UIContext> createNavigationContext();

/**
 * @brief Check if navigation system is active
 *
 * @return true if navigation system is initialized and has pages
 */
bool isNavigationActive();

/**
 * @brief Create main menu for navigation system
 *
 * Creates the main menu with Sample and System options,
 * integrating with existing functionality.
 *
 * @return Main menu page
 */
std::shared_ptr<UIPage> createMainMenu();

/**
 * @brief Create sample menu for navigation system
 *
 * Creates the sample menu with Record, Edit, Load/Save options.
 *
 * @return Sample menu page
 */
std::shared_ptr<UIPage> createSampleMenu();

/**
 * @brief Create system menu for navigation system
 *
 * Creates the system menu with Diagnostics and Settings options.
 *
 * @return System menu page
 */
std::shared_ptr<UIPage> createSystemMenu();

}  // namespace wavex_ui
