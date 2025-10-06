// WaveX UI Softkey Definition
#pragma once

#include <functional>
#include <string>
#include <array>

namespace wavex_ui {

/**
 * @brief Softkey definition for bottom navigation bar
 */
struct Softkey {
    std::string label;                    ///< Display text for the button
    std::function<void()> onPress;       ///< Callback when button is pressed
    bool enabled = true;                  ///< Whether button is enabled/clickable
};

/**
 * @brief Number of softkeys in the navigation bar
 */
constexpr int NUM_SOFTKEYS = 6;

} // namespace wavex_ui
