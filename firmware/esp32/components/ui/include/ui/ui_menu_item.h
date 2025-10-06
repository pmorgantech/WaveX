// WaveX UI Menu Item Definition
#pragma once

#include <functional>
#include <string>

namespace wavex_ui {

/**
 * @brief Menu item definition for hierarchical menus
 */
struct MenuItem {
    std::string label;                    ///< Display text for the menu item
    std::function<void()> onSelect;      ///< Callback when item is selected
};

} // namespace wavex_ui