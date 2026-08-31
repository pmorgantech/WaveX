// WaveX UI Navigation Integration
#pragma once

#include "input_dispatcher.h"
#include "ui_menu_page.h"
#include "ui_navigator.h"

#include <memory>

namespace wavex_ui {

// Glue between UINavigator and the input dispatcher/UI task.

// Sets up the navigation stack with the main menu as the root page. Call
// during UI initialization.
void initNavigationSystem();

// Routes an input event to the currently active page. Call from the input
// dispatcher.
void handleNavigationInput(const InputEvent& evt);

/// A UIContext that forwards input to the navigation system.
std::shared_ptr<UIContext> createNavigationContext();

bool isNavigationActive();

std::shared_ptr<UIPage> createMainMenu();

}  // namespace wavex_ui
