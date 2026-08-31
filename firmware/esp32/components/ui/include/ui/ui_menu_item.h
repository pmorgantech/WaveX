// WaveX UI Menu Item Definition
#pragma once

#include <functional>
#include <string>

namespace wavex_ui {

struct MenuItem {
    std::string label;
    std::function<void()> onSelect;
};

}  // namespace wavex_ui
